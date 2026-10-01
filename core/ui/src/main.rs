use gpui::{
    App, Application, Bounds, Context, Div, FocusHandle, KeyBinding, SharedString, Stateful, Task,
    TitlebarOptions, Window, WindowBounds, WindowOptions, actions, div, prelude::*, px, rgb, size,
};
use rex_launcher::{Action, HostContext, Options};
use rex_ui::{Controller, LAUNCH_WARNING, Phase, Readiness, Request, worker::Worker};
use std::sync::mpsc::TryRecvError;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::time::Duration;

actions!(
    rex_ui,
    [
        Doctor,
        Status,
        OfferLaunch,
        ConfirmLaunch,
        CancelLaunch,
        Quit
    ]
);

const HELP: &str = "RexPlayer native GPUI shell (Linux x86_64/aarch64)\n\
Usage: rex-player [--waydroid TRUSTED_PATH] [--timeout-ms 100..30000]\n\
The runtime must be provisioned separately. Opening the shell runs a read-only\n\
doctor check. Every Android UI request requires confirmation in the window.\n\
Keyboard: Ctrl-R Doctor, Ctrl-S Status, Ctrl-L request launch,\n\
Ctrl-Enter confirm shown launch prompt, Escape cancel, Ctrl-Q close.\n\
This shell does not embed or verify Android frames.";

struct RexPlayer {
    controller: Controller,
    worker: Option<Worker>,
    host_summary: String,
    focus: FocusHandle,
    _poll: Task<()>,
}

impl RexPlayer {
    fn new(
        window: &mut Window,
        cx: &mut Context<Self>,
        worker: Result<Worker, std::io::Error>,
        host_summary: String,
    ) -> Self {
        let mut controller = Controller::default();
        let worker = match worker {
            Ok(worker) => Some(worker),
            Err(error) => {
                controller.submit_failed(format!("Could not start runtime worker: {error}"));
                None
            }
        };
        let focus = cx.focus_handle();
        window.focus(&focus);
        let executor = cx.background_executor().clone();
        let poll = cx.spawn(async move |view, cx| {
            loop {
                executor.timer(Duration::from_millis(100)).await;
                if view.update(cx, |view, cx| view.drain_events(cx)).is_err() {
                    break;
                }
            }
        });
        let mut view = Self {
            controller,
            worker,
            host_summary,
            focus,
            _poll: poll,
        };
        if view.worker.is_some() {
            view.diagnose(Action::Doctor, cx);
        }
        view
    }

    fn submit(&mut self, request: Request, cx: &mut Context<Self>) {
        match self.worker.as_ref() {
            Some(worker) => {
                if let Err(error) = worker.submit(request) {
                    self.controller.submit_failed(error);
                }
            }
            None => self.controller.submit_failed(
                "Runtime worker is unavailable. Close and reopen RexPlayer to retry.",
            ),
        }
        cx.notify();
    }

    fn diagnose(&mut self, action: Action, cx: &mut Context<Self>) {
        if let Some(request) = self.controller.begin_diagnostic(action) {
            self.submit(request, cx);
        }
    }

    fn drain_events(&mut self, cx: &mut Context<Self>) {
        let mut changed = false;
        let mut disconnected = false;
        if let Some(worker) = &self.worker {
            // A fixed upper bound also keeps a faulty event source off the render path.
            for _ in 0..16 {
                match worker.try_event() {
                    Ok(event) => changed |= self.controller.apply(event),
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => {
                        disconnected = true;
                        break;
                    }
                }
            }
        }
        if disconnected {
            self.worker = None;
            self.controller.submit_failed("Runtime worker disconnected. The last result is no longer trusted. Close and reopen RexPlayer to retry.");
            changed = true;
        }
        if changed {
            cx.notify();
        }
    }

    fn offer(&mut self, cx: &mut Context<Self>) {
        if self.controller.offer_launch() {
            cx.notify();
        }
    }

    fn confirm(&mut self, cx: &mut Context<Self>) {
        if let Some(request) = self.controller.confirm_launch() {
            self.submit(request, cx);
        }
    }

    fn cancel(&mut self, cx: &mut Context<Self>) {
        self.controller.cancel_launch();
        cx.notify();
    }
}

fn button(id: &'static str, title: &'static str, enabled: bool, primary: bool) -> Stateful<Div> {
    div()
        .id(id)
        .px_4()
        .py_3()
        .rounded_md()
        .border_1()
        .border_color(rgb(if primary { 0x74d9aa } else { 0x354455 }))
        .bg(rgb(if primary { 0x173e33 } else { 0x18232f }))
        .text_color(rgb(if enabled { 0xe8f3ef } else { 0x77838b }))
        .text_sm()
        .when(enabled, |this| {
            this.cursor_pointer()
                .hover(|this| this.bg(rgb(if primary { 0x235a47 } else { 0x28394b })))
        })
        .child(title)
}

impl Render for RexPlayer {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let idle = self.controller.phase == Phase::Idle && self.worker.is_some();
        let can_launch = self.controller.can_offer_launch() && self.worker.is_some();
        let confirming = self.controller.phase == Phase::ConfirmLaunch;
        let (label, color) = match self.controller.phase {
            Phase::Checking(_) => ("CHECKING…", 0xe4c57c),
            Phase::ConfirmLaunch => ("CONFIRMATION REQUIRED", 0xe4c57c),
            Phase::Idle => match self.controller.readiness {
                Readiness::Ready => ("LAST READINESS CHECK PASSED", 0x86dfb3),
                Readiness::Unavailable => ("ACTION NEEDED", 0xf2a29d),
                Readiness::Unknown => ("READINESS NOT VERIFIED", 0xa5b7c6),
            },
        };
        div()
            .id("rex-player-root")
            .track_focus(&self.focus)
            .on_action(cx.listener(|this, _: &Doctor, _, cx| this.diagnose(Action::Doctor, cx)))
            .on_action(cx.listener(|this, _: &Status, _, cx| this.diagnose(Action::Status, cx)))
            .on_action(cx.listener(|this, _: &OfferLaunch, _, cx| this.offer(cx)))
            .on_action(cx.listener(|this, _: &ConfirmLaunch, _, cx| this.confirm(cx)))
            .on_action(cx.listener(|this, _: &CancelLaunch, _, cx| this.cancel(cx)))
            .on_action(|_: &Quit, window, _| window.remove_window())
            .size_full()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap_5()
            .p_8()
            .bg(rgb(0x0d151e))
            .text_color(rgb(0xe4eaf0))
            .child(
                div().flex().flex_col().gap_2()
                    .child(div().text_3xl().child("RexPlayer"))
                    .child(div().text_sm().text_color(rgb(0x9baebd))
                        .child("NATIVE WORKSPACE  /  WAYDROID"))
                    .child(div().text_sm().text_color(rgb(0x9baebd)).child(self.host_summary.clone())),
            )
            .child(
                div().flex().flex_col().gap_3().p_5().rounded_lg()
                    .border_1().border_color(rgb(0x283c4d)).bg(rgb(0x121f2b))
                    .child(div().text_sm().text_color(rgb(color)).child(label))
                    .children(self.controller.message.lines().map(|line| div().child(line.to_owned())))
                    .when_some(self.controller.last_exit_code, |this, code| {
                        this.child(div().text_sm().text_color(rgb(0x9baebd)).child(format!("Launcher result code: {code}")))
                    }),
            )
            .child(
                div().flex().flex_wrap().gap_3()
                    .child(button("doctor", "Doctor · Ctrl-R", idle, false)
                        .on_click(cx.listener(|this, _, _, cx| this.diagnose(Action::Doctor, cx))))
                    .child(button("status", "Read status · Ctrl-S", idle, false)
                        .on_click(cx.listener(|this, _, _, cx| this.diagnose(Action::Status, cx))))
                    .child(button("launch", "Open Android UI… · Ctrl-L", can_launch, true)
                        .on_click(cx.listener(|this, _, _, cx| this.offer(cx)))),
            )
            .when(confirming, |this| {
                this.child(
                    div().flex().flex_col().gap_4().p_5().rounded_lg()
                        .border_1().border_color(rgb(0xc69b54)).bg(rgb(0x302b21))
                        .child(div().text_lg().child("Allow this Waydroid UI request?"))
                        .child(LAUNCH_WARNING)
                        .child(div().text_sm().child("RexPlayer will check the current user, display, and RUNNING state again before requesting the UI."))
                        .child(
                            div().flex().flex_wrap().gap_3()
                                .child(button("cancel", "Cancel · Escape", true, false)
                                    .on_click(cx.listener(|this, _, _, cx| this.cancel(cx))))
                                .child(button("confirm", "Allow once & open · Ctrl-Enter", true, true)
                                    .on_click(cx.listener(|this, _, _, cx| this.confirm(cx)))),
                        ),
                )
            })
            .child(
                div().flex().flex_col().gap_2().p_5().rounded_lg().bg(rgb(0x141d26))
                    .child(div().text_sm().text_color(rgb(0x9baebd)).child("REQUEST PROCESS"))
                    .child(self.controller.request_process.clone()),
            )
            .child(
                div().flex().flex_col().gap_2().text_sm().text_color(rgb(0x9baebd))
                    .child("Uses your separately provisioned Linux Waydroid runtime. Doctor and Status are read-only.")
                    .child("Android opens in its own window. Embedded graphics, audio, app input, and rendered-frame verification are not implemented in this shell.")
                    .child("Closing RexPlayer does not stop an Android session. Ctrl-Q closes this window."),
            )
    }
}

fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() == 1 && (args[0] == "--help" || args[0] == "-h") {
        println!("{HELP}");
        return;
    }
    // Reuse the launcher's validated executable/timeout options. UI consent
    // cannot be pre-granted through a command-line flag or environment variable.
    let options = match Options::parse(std::iter::once("doctor".into()).chain(args)) {
        Ok(Some(options)) => options,
        Ok(None) => unreachable!("the injected doctor action never returns help"),
        Err(error) => {
            eprintln!("{error}\n{HELP}");
            std::process::exit(64);
        }
    };
    let host = HostContext::current();
    if host.os != "linux" || !matches!(host.arch.as_str(), "x86_64" | "aarch64") {
        eprintln!("This RexPlayer shell supports Linux x86_64/aarch64 only.");
        std::process::exit(69);
    }
    if std::env::var_os("WAYLAND_DISPLAY")
        .filter(|v| !v.is_empty())
        .is_none()
        && std::env::var_os("DISPLAY")
            .filter(|v| !v.is_empty())
            .is_none()
    {
        eprintln!(
            "No graphical display found. Run RexPlayer in your desktop session, or use rex-launcher doctor for headless diagnostics."
        );
        std::process::exit(69);
    }
    let host_summary = format!(
        "{} / {} · user {} · Wayland display {}",
        host.os,
        host.arch,
        host.effective_uid
            .map(|uid| uid.to_string())
            .unwrap_or_else(|| "unknown".into()),
        host.wayland_display.as_deref().unwrap_or("not set")
    );
    let worker = Worker::start(options.executable, options.timeout);
    let window_failed = Arc::new(AtomicBool::new(false));
    let failed = window_failed.clone();
    Application::new().run(move |cx: &mut App| {
        cx.bind_keys([
            KeyBinding::new("ctrl-r", Doctor, None),
            KeyBinding::new("ctrl-s", Status, None),
            KeyBinding::new("ctrl-l", OfferLaunch, None),
            KeyBinding::new("ctrl-enter", ConfirmLaunch, None),
            KeyBinding::new("escape", CancelLaunch, None),
            KeyBinding::new("ctrl-q", Quit, None),
        ]);
        cx.on_window_closed(|cx| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();
        let bounds = Bounds::centered(None, size(px(940.), px(740.)), cx);
        let result = cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                window_min_size: Some(size(px(680.), px(540.))),
                titlebar: Some(TitlebarOptions {
                    title: Some(SharedString::from("RexPlayer")),
                    ..Default::default()
                }),
                app_id: Some("org.rexplayer.RexPlayer".into()),
                ..Default::default()
            },
            |window, cx| cx.new(|cx| RexPlayer::new(window, cx, worker, host_summary)),
        );
        if let Err(error) = result {
            failed.store(true, Ordering::Relaxed);
            eprintln!("Could not open the RexPlayer window: {error}");
            cx.quit();
        } else {
            cx.activate(true);
        }
    });
    if window_failed.load(Ordering::Relaxed) {
        std::process::exit(70);
    }
}
