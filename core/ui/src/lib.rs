//! UI state and asynchronous process supervision, independent of GPUI rendering.
//!
//! Runtime interpretation and launch policy belong exclusively to rex-launcher.
//! The shell never provisions a runtime or stops an existing Android session.

use rex_launcher::{Action, Report};

pub mod input_gate;
pub mod keyboard;
pub mod options;
pub mod viewport;
pub mod worker;

pub const LAUNCH_WARNING: &str = "Waydroid's show-full-ui may start or unfreeze your session if its state changes after the check. Confirming grants this permission for this request only (equivalent to --allow-session-start). Closing RexPlayer will not stop Android.";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Idle,
    Checking(Action),
    ConfirmLaunch,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Readiness {
    Unknown,
    Ready,
    Unavailable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Request {
    pub id: u64,
    pub action: Action,
    pub allow_session_start: bool,
}

#[derive(Debug)]
pub enum Event {
    Finished { request: Request, report: Report },
    UiExited { pid: u32, code: Option<i32> },
    UiWaitFailed { pid: u32, message: String },
}

/// Guards repeated actions, one-shot consent, and stale asynchronous completions.
#[derive(Debug)]
pub struct Controller {
    pub phase: Phase,
    pub readiness: Readiness,
    pub message: String,
    pub request_process: String,
    pub last_exit_code: Option<i32>,
    pub ui_request_active: bool,
    current: Option<Request>,
    next_id: u64,
}

impl Default for Controller {
    fn default() -> Self {
        Self {
            phase: Phase::Idle,
            readiness: Readiness::Unknown,
            message: "Check the existing Waydroid runtime to begin.".into(),
            request_process: "No Android UI request made. Rendered frames are unverified.".into(),
            last_exit_code: None,
            ui_request_active: false,
            current: None,
            next_id: 0,
        }
    }
}

impl Controller {
    pub fn begin_diagnostic(&mut self, action: Action) -> Option<Request> {
        if self.phase != Phase::Idle || action == Action::Launch {
            return None;
        }
        // A later status or diagnostic invalidates the old readiness snapshot.
        self.readiness = Readiness::Unknown;
        Some(self.begin(action, false))
    }

    pub fn can_offer_launch(&self) -> bool {
        self.phase == Phase::Idle && self.readiness == Readiness::Ready && !self.ui_request_active
    }

    pub fn offer_launch(&mut self) -> bool {
        if !self.can_offer_launch() {
            return false;
        }
        self.phase = Phase::ConfirmLaunch;
        true
    }

    pub fn cancel_launch(&mut self) {
        if self.phase == Phase::ConfirmLaunch {
            self.phase = Phase::Idle;
        }
    }

    pub fn confirm_launch(&mut self) -> Option<Request> {
        if self.phase != Phase::ConfirmLaunch || self.ui_request_active {
            return None;
        }
        // There is no stored permission; each launch returns to the prompt.
        self.readiness = Readiness::Unknown;
        Some(self.begin(Action::Launch, true))
    }

    fn begin(&mut self, action: Action, allow_session_start: bool) -> Request {
        self.next_id = self
            .next_id
            .checked_add(1)
            .expect("request counter exhausted");
        let request = Request {
            id: self.next_id,
            action,
            allow_session_start,
        };
        self.current = Some(request);
        self.phase = Phase::Checking(action);
        self.last_exit_code = None;
        self.message = match action {
            Action::Status => "Reading Waydroid status…",
            Action::Doctor => "Checking this host and existing Waydroid session…",
            Action::Launch => "Rechecking readiness, then requesting the Android UI…",
        }
        .into();
        request
    }

    pub fn submit_failed(&mut self, message: impl Into<String>) {
        self.current = None;
        self.phase = Phase::Idle;
        self.readiness = Readiness::Unavailable;
        self.last_exit_code = Some(70);
        self.message = message.into();
    }

    pub fn apply(&mut self, event: Event) -> bool {
        match event {
            Event::Finished { request, report } => {
                if self.current != Some(request) {
                    return false;
                }
                self.current = None;
                self.phase = Phase::Idle;
                self.last_exit_code = Some(report.exit_code);
                self.message = report.message;
                self.readiness = if request.action == Action::Doctor && report.exit_code == 0 {
                    Readiness::Ready
                } else if report.exit_code != 0 {
                    Readiness::Unavailable
                } else {
                    Readiness::Unknown
                };
                if request.action == Action::Launch && report.exit_code == 0 {
                    self.ui_request_active = true;
                    self.request_process = "UI request spawned; waiting for the request process to exit. No rendered-frame verification.".into();
                }
            }
            Event::UiExited { pid, code } => {
                self.ui_request_active = false;
                self.readiness = if code == Some(0) {
                    Readiness::Unknown
                } else {
                    Readiness::Unavailable
                };
                let outcome = match code {
                    Some(0) => "completed (exit 0)".to_owned(),
                    Some(code) => format!("failed (exit {code}); inspect its terminal diagnostics"),
                    None => "was terminated by a signal".to_owned(),
                };
                self.request_process = format!(
                    "UI request process {pid} {outcome}. This does not verify a visible Android window. Run Doctor before another request."
                );
            }
            Event::UiWaitFailed { pid, message } => {
                // Keep repeated launch blocked while ownership/status is uncertain.
                self.ui_request_active = true;
                self.readiness = Readiness::Unavailable;
                self.request_process = format!(
                    "Cannot establish UI request process {pid} status: {message}. Further UI requests are blocked."
                );
            }
        }
        true
    }
}
