use gpui::{
    App, Application, Bounds, Context, Div, FocusHandle, KeyBinding, KeyDownEvent, KeyUpEvent,
    ModifiersChangedEvent, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, ObjectFit,
    RenderImage, SharedString, Stateful, Task, TitlebarOptions, Window, WindowBounds,
    WindowOptions, actions, canvas, div, img, prelude::*, px, rgb, size,
};
use rex_input_core::{AndroidSize, Rotation};
use rex_keymap::{
    HostEvent, Keymap, MouseButton as HostMouseButton, WindowViewport,
    worker::{InputWorker, WorkerState},
};
use rex_launcher::{Action, HostContext};
use rex_media::{AUDIO_NOTICE, AudioState, MediaHandle, MediaSession, VideoBackend, VideoState};
use rex_ui::input_gate::{self, Dispatch};
use rex_ui::keyboard::{KeyDecision, NativeKeyboard, validate_native_keymap};
use rex_ui::{
    Controller, LAUNCH_WARNING, Phase, Request, options::UiOptions, viewport::ContentRect,
    worker::Worker,
};
use std::io::Read;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
    mpsc::TryRecvError,
};
use std::time::{Duration, Instant};

actions!(
    rex_ui,
    [Doctor, Status, OfferLaunch, ConfirmLaunch, Escape, Quit]
);
const INPUT_NOTICE: &str = "Create a Linux virtual touchscreen and send this window's mapped keys/mouse to it. This host device is NOT automatically bound to the selected ADB device. Guest routing must already be configured; otherwise events may affect the host desktop. No permission or routing settings are changed. Escape, losing focus, resizing, stopping capture, or a geometry change disables input and requests release of owned contacts. Enable only when you have verified the guest route.";
const STALE_AFTER: Duration = Duration::from_secs(2);
type InputOwner = Arc<Mutex<Option<Arc<InputWorker>>>>;
#[derive(Clone, Copy, PartialEq)]
enum Prompt {
    Audio,
    Input,
}

struct RexPlayer {
    controller: Controller,
    worker: Option<Worker>,
    host_summary: String,
    media: Option<MediaHandle>,
    backend: VideoBackend,
    media_revision: u64,
    video: VideoState,
    audio: AudioState,
    audio_diagnostics: String,
    image: Option<Arc<RenderImage>>,
    frame_size: Option<(u32, u32)>,
    last_frame: Option<Instant>,
    stale: bool,
    frame_description: String,
    measured_rect: Arc<Mutex<Option<ContentRect>>>,
    input_rect: Option<ContentRect>,
    input: Option<Arc<InputWorker>>,
    input_owner: InputOwner,
    input_message: String,
    keymap: Keymap,
    keyboard: NativeKeyboard,
    keymap_name: String,
    prompt: Option<Prompt>,
    focus: FocusHandle,
    _poll: Task<()>,
}

impl RexPlayer {
    #[allow(clippy::too_many_arguments)]
    fn new(
        window: &mut Window,
        cx: &mut Context<Self>,
        worker: Result<Worker, std::io::Error>,
        host_summary: String,
        media: Option<MediaHandle>,
        backend: VideoBackend,
        keymap: Keymap,
        keymap_name: String,
        input_owner: InputOwner,
    ) -> Self {
        let mut controller = Controller::default();
        let worker = match worker {
            Ok(worker) => Some(worker),
            Err(error) => {
                controller.submit_failed(format!("Waydroid worker unavailable: {error}"));
                None
            }
        };
        let focus = cx.focus_handle();
        window.focus(&focus);
        let executor = cx.background_executor().clone();
        let poll = cx.spawn(async move |view, cx| {
            loop {
                executor.timer(Duration::from_millis(20)).await;
                if view.update(cx, |view, cx| view.poll(cx)).is_err() {
                    break;
                }
            }
        });
        cx.observe_window_activation(window, |view, window, cx| {
            if !window.is_window_active() {
                view.disable_input("Window lost focus", cx);
            }
        })
        .detach();
        cx.observe_window_bounds(window, |view, _, cx| {
            view.disable_input(
                "Window resized or moved; enable input again after checking the viewport",
                cx,
            );
        })
        .detach();
        let layout_view = cx.entity().downgrade();
        cx.on_keyboard_layout_change(move |cx| {
            let _ = layout_view.update(cx, |view, cx| {
                view.disable_input("Keyboard layout changed", cx)
            });
        })
        .detach();
        cx.on_release(|view, cx| {
            if let Some(media) = &view.media {
                media.shutdown();
            }
            if let Some(input) = &view.input {
                input.shutdown();
            }
            if let Some(image) = view.image.take() {
                cx.drop_image(image, None);
            }
        })
        .detach();
        let auto_doctor = media.is_none();
        let mut view = Self {
            controller,
            worker,
            host_summary,
            media,
            backend,
            media_revision: 0,
            video: VideoState::Stopped,
            audio: AudioState::Stopped,
            audio_diagnostics: String::new(),
            image: None,
            frame_size: None,
            last_frame: None,
            stale: false,
            frame_description: "No device frame received".into(),
            measured_rect: Arc::new(Mutex::new(None)),
            input_rect: None,
            input: None,
            input_owner,
            input_message: "Input disabled; no virtual device has been created".into(),
            keymap,
            keyboard: NativeKeyboard::default(),
            keymap_name,
            prompt: None,
            focus,
            _poll: poll,
        };
        if auto_doctor && view.worker.is_some() {
            view.diagnose(Action::Doctor, cx);
        }
        view
    }

    fn submit(&mut self, request: Request, cx: &mut Context<Self>) {
        match &self.worker {
            Some(worker) => {
                if let Err(error) = worker.submit(request) {
                    self.controller.submit_failed(error);
                }
            }
            None => self
                .controller
                .submit_failed("Waydroid worker is unavailable; restart RexPlayer to retry"),
        }
        cx.notify();
    }
    fn diagnose(&mut self, action: Action, cx: &mut Context<Self>) {
        if self.prompt.is_some() {
            return;
        }
        if let Some(request) = self.controller.begin_diagnostic(action) {
            self.submit(request, cx);
        }
    }
    fn offer_launch(&mut self, cx: &mut Context<Self>) {
        if self.prompt.is_none() && self.controller.offer_launch() {
            self.disable_input("Launch confirmation opened", cx);
            cx.notify();
        }
    }
    fn confirm_launch(&mut self, cx: &mut Context<Self>) {
        if let Some(request) = self.controller.confirm_launch() {
            self.submit(request, cx);
        }
    }
    fn escape(&mut self, cx: &mut Context<Self>) {
        self.prompt = None;
        self.controller.cancel_launch();
        self.disable_input("Escape: input disabled and cleanup requested", cx);
        cx.notify();
    }
    fn clear_image(&mut self, cx: &mut Context<Self>) {
        if let Some(image) = self.image.take() {
            cx.drop_image(image, None);
        }
        self.last_frame = None;
        self.frame_size = None;
        if let Ok(mut rect) = self.measured_rect.lock() {
            *rect = None;
        }
    }
    fn start_capture(&mut self, cx: &mut Context<Self>) {
        if self.prompt.is_some()
            || self.controller.phase == Phase::ConfirmLaunch
            || !matches!(self.video, VideoState::Stopped | VideoState::Failed(_))
        {
            return;
        }
        self.disable_input("Starting a new capture requires explicit input enable", cx);
        self.clear_image(cx);
        if let Some(media) = &self.media
            && media.start_video()
        {
            self.video = VideoState::Starting;
        }
        cx.notify();
    }
    fn stop_player(&mut self, cx: &mut Context<Self>) {
        self.prompt = None;
        self.disable_input("Player stopped; input cleanup requested", cx);
        if let Some(media) = &self.media {
            media.stop_video();
            media.stop_audio();
        }
        self.clear_image(cx);
        cx.notify();
    }
    fn fresh_frame(&self) -> bool {
        self.image.is_some()
            && self
                .last_frame
                .is_some_and(|time| time.elapsed() <= STALE_AFTER)
            && matches!(self.video, VideoState::Capturing { .. })
    }
    fn input_finished(&self) -> bool {
        self.input
            .as_ref()
            .is_none_or(|input| input.wait_closed(Duration::ZERO))
    }
    fn open_prompt(&mut self, prompt: Prompt, cx: &mut Context<Self>) {
        if self.media.is_none()
            || self.prompt.is_some()
            || self.controller.phase == Phase::ConfirmLaunch
        {
            return;
        }
        if prompt == Prompt::Input && (!self.fresh_frame() || !self.input_finished()) {
            return;
        }
        if prompt == Prompt::Audio
            && !matches!(self.audio, AudioState::Stopped | AudioState::Failed(_))
        {
            return;
        }
        self.disable_input("Confirmation opened; input disabled", cx);
        self.prompt = Some(prompt);
        cx.notify();
    }
    fn confirm_prompt(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        match self.prompt.take() {
            Some(Prompt::Audio) => {
                if let Some(media) = &self.media
                    && media.start_audio() {
                        self.audio = AudioState::Starting;
                    }
            }
            Some(Prompt::Input)
                if self.fresh_frame() && self.input_finished() && window.is_window_active() && !window.modifiers().modified() =>
            {
                let rect = self.measured_rect.lock().ok().and_then(|r| *r);
                if let (Some(rect), Some((width, height))) = (rect, self.frame_size) {
                    let viewport =
                        WindowViewport::new(rect.left, rect.top, rect.width, rect.height);
                    let size = AndroidSize::new(width, height);
                    match (viewport, size) {
                        (Ok(viewport), Ok(size)) => match InputWorker::spawn(
                            self.keymap.clone(),
                            viewport,
                            size,
                            Rotation::None,
                        ) {
                            Ok(worker) => {
                                let worker = Arc::new(worker);
                                if let Err(error) = worker.send(HostEvent::FocusGained) {
                                    worker.shutdown();
                                    self.input_message = error.to_string();
                                } else {
                                    self.input_message =
                                        "Creating explicitly requested virtual touchscreen…".into();
                                }
                                if let Ok(mut owner) = self.input_owner.lock() {
                                    *owner = Some(worker.clone());
                                }
                                self.keyboard.clear();
                                self.input = Some(worker);
                                self.input_rect = Some(rect);
                            }
                            Err(error) => {
                                self.input_message =
                                    format!("Input worker could not start: {error}")
                            }
                        },
                        _ => {
                            self.input_message =
                                "Invalid displayed geometry; input remains disabled".into()
                        }
                    }
                } else {
                    self.input_message =
                        "Displayed geometry is not ready; input remains disabled".into();
                }
            }
            Some(Prompt::Input) => self.input_message = "Input remains disabled. Release modifier keys and wait for a fresh frame and completed cleanup before enabling.".into(),
            _ => {}
        }
        cx.notify();
    }
    fn disable_input(&mut self, reason: &str, cx: &mut Context<Self>) {
        if let Some(input) = &self.input
            && !input.wait_closed(Duration::ZERO)
        {
            input.shutdown();
            self.input_message = format!("{reason}. Waiting for owned-device cleanup");
        }
        self.input_rect = None;
        self.keyboard.clear();
        cx.notify();
    }
    fn input_event(&mut self, event: HostEvent, window: &mut Window, cx: &mut Context<Self>) {
        let rect = self.measured_rect.lock().ok().and_then(|r| *r);
        let gate = input_gate::window_gate(
            self.input_rect.is_some(),
            window.is_window_active(),
            self.fresh_frame(),
            self.prompt.is_some() || self.controller.phase == Phase::ConfirmLaunch,
            rect == self.input_rect,
        );
        let Some(input) = self.input.clone() else {
            return;
        };
        // The nonblocking sender is authoritative. Never discard KeyUp/MouseUp
        // because status() temporarily returned None while publishing a snapshot.
        match input_gate::dispatch(gate, event, |event| input.send(event), || input.shutdown()) {
            Ok(Dispatch::Closed) => {
                self.disable_input("Input prerequisite changed; cleanup requested", cx)
            }
            Err(error) => {
                self.disable_input("Input event could not be queued", cx);
                self.input_message = error.to_string();
            }
            _ => {}
        }
    }
    fn apply_key(&mut self, decision: KeyDecision, window: &mut Window, cx: &mut Context<Self>) {
        match decision {
            KeyDecision::Ignore => {}
            KeyDecision::Event(event) => self.input_event(event, window, cx),
            KeyDecision::ReleaseAll => self.disable_input(
                "Keyboard identity/modifier changed; input disabled safely",
                cx,
            ),
        }
    }
    fn key_down(&mut self, event: &KeyDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        if event.keystroke.key == "escape" {
            if !event.is_held {
                self.escape(cx);
            }
            cx.stop_propagation();
            return;
        }
        if self.input_rect.is_none() {
            return;
        }
        let decision = self.keyboard.press(
            &event.keystroke.key,
            event.is_held,
            event.keystroke.modifiers.modified(),
        );
        self.apply_key(decision, window, cx);
    }
    fn key_up(&mut self, event: &KeyUpEvent, window: &mut Window, cx: &mut Context<Self>) {
        if self.input_rect.is_none() {
            return;
        }
        let decision = self.keyboard.release(&event.keystroke.key);
        self.apply_key(decision, window, cx);
    }
    fn modifiers_changed(
        &mut self,
        _: &ModifiersChangedEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let decision = self.keyboard.modifiers_or_layout_changed();
        self.apply_key(decision, window, cx);
    }
    fn mouse_down(&mut self, event: &MouseDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        let (x, y) = (
            f64::from(f32::from(event.position.x)),
            f64::from(f32::from(event.position.y)),
        );
        let rect = self.measured_rect.lock().ok().and_then(|r| *r);
        if !rect.is_some_and(|r| r.contains(x, y)) {
            return;
        }
        if let Some(button) = host_button(event.button) {
            self.input_event(HostEvent::MouseDown { button, x, y }, window, cx);
        }
    }
    fn mouse_move(&mut self, event: &MouseMoveEvent, window: &mut Window, cx: &mut Context<Self>) {
        if event.pressed_button.is_none() {
            return;
        }
        self.input_event(
            HostEvent::MouseMove {
                x: f64::from(f32::from(event.position.x)),
                y: f64::from(f32::from(event.position.y)),
            },
            window,
            cx,
        );
    }
    fn mouse_up(&mut self, event: &MouseUpEvent, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(button) = host_button(event.button) {
            self.input_event(HostEvent::MouseUp { button }, window, cx);
        }
    }
    fn poll(&mut self, cx: &mut Context<Self>) {
        let mut changed = false;
        let mut disconnected = false;
        if let Some(worker) = &self.worker {
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
            self.controller
                .submit_failed("Waydroid worker disconnected; restart to retry");
            changed = true;
        }
        if let Some(update) = self
            .media
            .as_ref()
            .and_then(|m| m.take_update(&mut self.media_revision))
        {
            self.video = update.video;
            self.audio = update.audio;
            self.audio_diagnostics = update.audio_diagnostics;
            if let Some(frame) = update.frame {
                if self.frame_size != Some((frame.width, frame.height)) {
                    self.disable_input("Android dimensions/orientation changed", cx);
                }
                self.frame_size = Some((frame.width, frame.height));
                self.last_frame = Some(frame.captured_at);
                self.frame_description = format!(
                    "Frame {} · {} × {} · {:.0} ms capture/decode · CPU-copy {}",
                    frame.number,
                    frame.width,
                    frame.height,
                    frame.capture_duration.as_secs_f64() * 1000.0,
                    if self.backend == VideoBackend::Screenshot {
                        "PNG polling"
                    } else {
                        "H264/FFmpeg stream"
                    }
                );
                if let Some(buffer) =
                    image::RgbaImage::from_raw(frame.width, frame.height, frame.bgra)
                {
                    let new_image = Arc::new(RenderImage::new(vec![image::Frame::new(buffer)]));
                    if let Some(old) = self.image.replace(new_image) {
                        cx.drop_image(old, None);
                    }
                }
            }
            if matches!(
                self.video,
                VideoState::Stopped | VideoState::Stopping | VideoState::Failed(_)
            ) {
                self.disable_input("Capture is not active", cx);
                self.clear_image(cx);
            }
            changed = true;
        }
        let stale = self
            .last_frame
            .is_some_and(|time| time.elapsed() > STALE_AFTER);
        if stale != self.stale {
            self.stale = stale;
            changed = true;
        }
        if self.input_rect.is_some() && !self.fresh_frame() {
            self.disable_input("No fresh decoded frame for two seconds", cx);
            changed = true;
        }
        if let Some(input) = &self.input
            && let Some(status) = input.status()
        {
            let message = format!(
                "Input {:?} · contacts {:?}. {}",
                status.state,
                status.active_contacts,
                status.message.unwrap_or_default()
            );
            if message != self.input_message {
                self.input_message = message;
                changed = true;
            }
        }
        let rect = self.measured_rect.lock().ok().and_then(|r| *r);
        if self.input_rect.is_some() && self.input_rect != rect {
            self.disable_input("Content viewport changed", cx);
            changed = true;
        }
        if changed {
            cx.notify();
        }
    }
}

fn host_button(button: MouseButton) -> Option<HostMouseButton> {
    match button {
        MouseButton::Left => Some(HostMouseButton::Left),
        MouseButton::Right => Some(HostMouseButton::Right),
        MouseButton::Middle => Some(HostMouseButton::Middle),
        _ => None,
    }
}
fn button(id: &'static str, label: &'static str, enabled: bool, primary: bool) -> Stateful<Div> {
    div()
        .id(id)
        .px_4()
        .py_2()
        .rounded_md()
        .border_1()
        .border_color(rgb(if primary { 0x74d9aa } else { 0x354455 }))
        .bg(rgb(if primary { 0x173e33 } else { 0x18232f }))
        .text_color(rgb(if enabled { 0xe8f3ef } else { 0x77838b }))
        .text_sm()
        .when(enabled, |this| {
            this.cursor_pointer().hover(|this| this.bg(rgb(0x28394b)))
        })
        .child(label)
}

impl Render for RexPlayer {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let idle =
            self.controller.phase == Phase::Idle && self.worker.is_some() && self.prompt.is_none();
        let can_capture = self.media.is_some()
            && matches!(self.video, VideoState::Stopped | VideoState::Failed(_))
            && self.prompt.is_none();
        let can_audio = self.media.is_some()
            && matches!(self.audio, AudioState::Stopped | AudioState::Failed(_))
            && self.prompt.is_none();
        let can_input = self.fresh_frame() && self.input_finished() && self.prompt.is_none();
        let measure = self.measured_rect.clone();
        let frame_size = self.frame_size;
        let serial = self
            .media
            .as_ref()
            .map(|m| m.serial().to_owned())
            .unwrap_or_else(|| "No device selected. Start with --adb-serial SERIAL".into());
        let video_label = match &self.video {
            VideoState::Stopped => "Capture stopped".into(),
            VideoState::Stopping => "Stopping capture; waiting for owned process cleanup…".into(),
            VideoState::Starting => "Waiting for the first decoded frame…".into(),
            VideoState::Capturing { .. }
                if self
                    .last_frame
                    .is_some_and(|time| time.elapsed() > STALE_AFTER) =>
            {
                "Last frame is stale; mapped input is disabled".into()
            }
            VideoState::Capturing { .. } => self.frame_description.clone(),
            VideoState::Failed(error) => format!("Capture error: {error}"),
        };
        let audio_label = match &self.audio {
            AudioState::Stopped => "Audio stopped".into(),
            AudioState::Stopping => "Stopping audio; cleanup pending…".into(),
            AudioState::Starting => "Starting audio-only scrcpy…".into(),
            AudioState::ProcessRunning { pid } => {
                format!("Audio process {pid} running; audible playback unverified")
            }
            AudioState::Failed(error) => format!("Audio error: {error}"),
        };
        div().id("rex-player-root").track_focus(&self.focus)
            .capture_key_down(cx.listener(Self::key_down)).on_key_up(cx.listener(Self::key_up))
            .on_modifiers_changed(cx.listener(Self::modifiers_changed))
            .on_any_mouse_down(cx.listener(Self::mouse_down)).on_mouse_move(cx.listener(Self::mouse_move))
            .capture_any_mouse_up(cx.listener(Self::mouse_up))
            .on_mouse_up_out(MouseButton::Left, cx.listener(Self::mouse_up))
            .on_mouse_up_out(MouseButton::Right, cx.listener(Self::mouse_up))
            .on_mouse_up_out(MouseButton::Middle, cx.listener(Self::mouse_up))
            .on_action(cx.listener(|this, _: &Doctor, _, cx| this.diagnose(Action::Doctor, cx)))
            .on_action(cx.listener(|this, _: &Status, _, cx| this.diagnose(Action::Status, cx)))
            .on_action(cx.listener(|this, _: &OfferLaunch, _, cx| this.offer_launch(cx)))
            .on_action(cx.listener(|this, _: &ConfirmLaunch, _, cx| this.confirm_launch(cx)))
            .on_action(cx.listener(|this, _: &Escape, _, cx| this.escape(cx)))
            .on_action(cx.listener(|this, _: &Quit, window, cx| { this.stop_player(cx); window.remove_window(); }))
            .size_full().overflow_y_scroll().flex().flex_col().gap_4().p_6()
            .bg(rgb(0x0d151e)).text_color(rgb(0xe4eaf0))
            .child(div().flex().flex_col().gap_1().flex_shrink_0()
                .child(div().text_3xl().child("RexPlayer"))
                .child(div().text_sm().text_color(rgb(0x9baebd)).child("INTEGRATED ANDROID COMPATIBILITY PLAYER"))
                .child(div().text_sm().text_color(rgb(0x9baebd)).child(self.host_summary.clone())))
            .child(div().flex().flex_col().gap_3().flex_shrink_0()
                .child(format!("Selected device: {serial}"))
                .child(div().flex().flex_wrap().gap_2()
                    .child(button("capture", "Start capture", can_capture, true).on_click(cx.listener(|this, _, _, cx| this.start_capture(cx))))
                    .child(button("stop", "Stop player", self.media.is_some(), false).on_click(cx.listener(|this, _, _, cx| this.stop_player(cx))))
                    .child(button("audio", "Start audio…", can_audio, false).on_click(cx.listener(|this, _, _, cx| this.open_prompt(Prompt::Audio, cx))))
                    .child(button("stop-audio", "Stop audio", self.media.is_some(), false).on_click(cx.listener(|this, _, _, cx| { if let Some(media) = &this.media { media.stop_audio(); } cx.notify(); })))
                    .child(button("input", "Enable mapped input…", can_input, false).on_click(cx.listener(|this, _, _, cx| this.open_prompt(Prompt::Input, cx))))
                    .child(button("release", "Release / disable input", self.input.is_some(), false).on_click(cx.listener(|this, _, _, cx| this.disable_input("Input disabled by request", cx)))))
                .child(div().text_sm().text_color(rgb(0x9baebd)).child("Capture may start your local ADB server. No device discovery or connection settings are changed.")))
            .child(div().id("android-viewport").relative().h(px(440.)).w_full().flex_shrink_0().overflow_hidden()
                .rounded_lg().border_1().border_color(rgb(0x283c4d)).bg(rgb(0x05080b))
                .when_some(self.image.clone(), |this, image| this.child(img(image).size_full().object_fit(ObjectFit::Contain)))
                .when(self.image.is_none(), |this| this.child(div().size_full().flex().items_center().justify_center().p_8().text_color(rgb(0x91a7b8)).child(video_label.clone())))
                .child(canvas(move |bounds, _, _| {
                    let rect = ContentRect { left: f64::from(f32::from(bounds.origin.x)), top: f64::from(f32::from(bounds.origin.y)), width: f64::from(f32::from(bounds.size.width)), height: f64::from(f32::from(bounds.size.height)) };
                    if let Ok(mut measured) = measure.lock() { *measured = frame_size.and_then(|(w,h)| rect.contain(w,h)); }
                }, |_, _, _, _| {}).absolute().size_full()))
            .child(div().flex().flex_col().gap_2().flex_shrink_0().text_sm()
                .child(video_label).child(audio_label)
                .when(!self.audio_diagnostics.is_empty(), |this| this.child(self.audio_diagnostics.clone()))
                .child(format!("Keymap: {}. {}", self.keymap_name, self.input_message))
                .child(div().text_color(rgb(0x9baebd)).child("Escape disables input immediately. Blur, resize, stale frames, and orientation changes require manual re-enable. Guest routing is unverified.")))
            .when_some(self.prompt, |this, prompt| this.child(div().flex().flex_col().gap_3().p_4().flex_shrink_0().rounded_lg().border_1().border_color(rgb(0xc69b54)).bg(rgb(0x302b21))
                .child(if prompt == Prompt::Audio { "Allow device-output audio?" } else { "Enable host virtual touchscreen?" })
                .child(if prompt == Prompt::Audio { AUDIO_NOTICE } else { INPUT_NOTICE })
                .child(div().flex().gap_3()
                    .child(button("cancel-media", "Cancel · Escape", true, false).on_click(cx.listener(|this, _, _, cx| this.escape(cx))))
                    .child(button("confirm-media", "Allow this session", true, true).on_click(cx.listener(|this, _, window, cx| this.confirm_prompt(window, cx)))))))
            .child(div().flex().flex_col().gap_3().p_4().flex_shrink_0().rounded_lg().bg(rgb(0x141f2b))
                .child("Optional Waydroid launcher")
                .children(self.controller.message.lines().map(|line| div().text_sm().child(line.to_owned())))
                .child(div().flex().flex_wrap().gap_2()
                    .child(button("doctor", "Doctor · Ctrl-R", idle, false).on_click(cx.listener(|this, _, _, cx| this.diagnose(Action::Doctor, cx))))
                    .child(button("status", "Read status · Ctrl-S", idle, false).on_click(cx.listener(|this, _, _, cx| this.diagnose(Action::Status, cx))))
                    .child(button("launch", "Open separate Android UI…", self.controller.can_offer_launch(), false).on_click(cx.listener(|this, _, _, cx| this.offer_launch(cx)))))
                .when(self.controller.phase == Phase::ConfirmLaunch, |this| this.child(div().flex().flex_col().gap_2()
                    .child(LAUNCH_WARNING)
                    .child(div().flex().gap_2()
                        .child(button("cancel-launch", "Cancel · Escape", true, false).on_click(cx.listener(|this, _, _, cx| this.escape(cx))))
                        .child(button("confirm-launch", "Allow once & open · Ctrl-Enter", true, true).on_click(cx.listener(|this, _, _, cx| this.confirm_launch(cx)))))))
                .child(div().text_sm().child(self.controller.request_process.clone())))
            .child(div().text_sm().flex_shrink_0().text_color(rgb(0x9baebd)).child("This is a CPU-copy compatibility pipeline. Actual Android rendering, audible playback, input routing, performance and hardware support require supported-host validation. Closing requests cleanup of owned capture/audio/input workers; it does not stop the Android runtime."))
    }
}

fn load_keymap(path: Option<&Path>) -> Result<(Keymap, String), String> {
    match path {
        None => Ok((Keymap::default(), "built-in default".into())),
        Some(path) => {
            let file = std::fs::OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
                .open(path)
                .map_err(|e| format!("Could not open keymap: {e}"))?;
            if !file.metadata().map_err(|e| e.to_string())?.is_file() {
                return Err("Keymap must be a regular JSON file".into());
            }
            let map = Keymap::from_reader(file.take((rex_keymap::MAX_PROFILE_BYTES + 1) as u64))
                .map_err(|e| e.to_string())?;
            validate_native_keymap(&map)?;
            Ok((
                map,
                path.file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .into_owned(),
            ))
        }
    }
}

pub fn run(options: UiOptions) {
    let host = HostContext::current();
    if !matches!(host.arch.as_str(), "x86_64" | "aarch64")
        || host.effective_uid.is_none_or(|uid| uid == 0)
    {
        eprintln!("RexPlayer requires an ordinary non-root Linux x86_64/aarch64 user.");
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
    let (keymap, keymap_name) = match load_keymap(options.keymap.as_deref()) {
        Ok(value) => value,
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(64);
        }
    };
    let backend = options
        .media
        .as_ref()
        .map(|config| config.video_backend)
        .unwrap_or(VideoBackend::Screenshot);
    let mut media_session = match options.media.map(MediaSession::new).transpose() {
        Ok(session) => session,
        Err(error) => {
            eprintln!("Could not initialize media workers: {error}");
            std::process::exit(70);
        }
    };
    let media = media_session.as_ref().map(MediaSession::handle);
    let host_summary = format!(
        "{} / {} · user {} · Wayland {}",
        host.os,
        host.arch,
        host.effective_uid.unwrap(),
        host.wayland_display.as_deref().unwrap_or("not set")
    );
    let worker = Worker::start(options.launcher.executable, options.launcher.timeout);
    let window_failed = Arc::new(AtomicBool::new(false));
    let failed = window_failed.clone();
    let input_owner: InputOwner = Arc::new(Mutex::new(None));
    let view_input_owner = input_owner.clone();
    Application::new().run(move |cx: &mut App| {
        cx.bind_keys([
            KeyBinding::new("ctrl-r", Doctor, None),
            KeyBinding::new("ctrl-s", Status, None),
            KeyBinding::new("ctrl-l", OfferLaunch, None),
            KeyBinding::new("ctrl-enter", ConfirmLaunch, None),
            KeyBinding::new("escape", Escape, None),
            KeyBinding::new("ctrl-q", Quit, None),
        ]);
        cx.on_window_closed(|cx| {
            // GPUI 0.2.2's X11 WM_DELETE_WINDOW handler holds its client-state
            // RefCell borrow while invoking this observer. Even App::defer runs
            // before that platform callback unwinds. Queue a foreground task so
            // Platform::quit cannot reborrow the client until the next dispatch.
            cx.spawn(async |cx| {
                let _ = cx.update(|cx| {
                    // Another window may have opened before this task runs.
                    if cx.windows().is_empty() {
                        cx.quit();
                    }
                });
            })
            .detach();
        })
        .detach();
        let bounds = Bounds::centered(None, size(px(1100.), px(920.)), cx);
        let result = cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                window_min_size: Some(size(px(720.), px(620.))),
                titlebar: Some(TitlebarOptions {
                    title: Some(SharedString::from("RexPlayer")),
                    ..Default::default()
                }),
                app_id: Some("org.rexplayer.RexPlayer".into()),
                ..Default::default()
            },
            |window, cx| {
                cx.new(|cx| {
                    RexPlayer::new(
                        window,
                        cx,
                        worker,
                        host_summary,
                        media,
                        backend,
                        keymap,
                        keymap_name,
                        view_input_owner,
                    )
                })
            },
        );
        if let Err(error) = result {
            failed.store(true, Ordering::Release);
            eprintln!("Could not open RexPlayer: {error}");
            cx.quit();
        } else {
            cx.activate(true);
        }
    });
    let mut failed = window_failed.load(Ordering::Acquire);
    // All bounded waits happen after GPUI's event loop has ended, never in it.
    if let Some(session) = &mut media_session
        && let Err(error) = session.close(Duration::from_secs(3))
    {
        eprintln!("Media shutdown: {error}");
        failed = true;
    }
    if let Some(input) = input_owner.lock().ok().and_then(|owner| owner.clone()) {
        input.shutdown();
        if !input.wait_closed(Duration::from_secs(2)) {
            eprintln!("Input cleanup did not finish; release delivery is unverified");
            failed = true;
        } else if input
            .status()
            .is_some_and(|s| s.state == WorkerState::Failed)
        {
            eprintln!("Input worker ended with an error; inspect its diagnostics");
            failed = true;
        }
    }
    if failed {
        std::process::exit(70);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::ImageEncoder;

    #[test]
    fn fifo_keymap_is_rejected_without_waiting_for_a_writer() {
        let path = std::env::temp_dir().join(format!("rex-ui-keymap-fifo-{}", std::process::id()));
        let status = std::process::Command::new("mkfifo")
            .arg(&path)
            .status()
            .unwrap();
        assert!(status.success());
        let started = Instant::now();
        let result = load_keymap(Some(&path));
        let _ = std::fs::remove_file(path);
        assert!(started.elapsed() < Duration::from_secs(1));
        assert!(result.unwrap_err().contains("regular JSON file"));
    }

    #[test]
    fn actual_gpui_image_accepts_decoded_bgra_without_channel_or_dimension_change() {
        let mut bytes = Vec::new();
        image::codecs::png::PngEncoder::new(&mut bytes)
            .write_image(&[250, 30, 10, 255], 1, 1, image::ExtendedColorType::Rgba8)
            .unwrap();
        let frame = rex_media::decode_png(&bytes, 1, Instant::now()).unwrap();
        let buffer = image::RgbaImage::from_raw(frame.width, frame.height, frame.bgra).unwrap();
        let image = RenderImage::new(vec![image::Frame::new(buffer)]);
        assert_eq!(image.as_bytes(0).unwrap(), &[10, 30, 250, 255]);
        assert_eq!(u32::from(image.size(0).width), 1);
        assert_eq!(u32::from(image.size(0).height), 1);
    }
}
