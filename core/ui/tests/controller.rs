use rex_launcher::{Action, Report};
use rex_ui::{Controller, Event, Phase, Readiness, Request};

fn finish(controller: &mut Controller, request: Request, code: i32) {
    assert!(controller.apply(Event::Finished {
        request,
        report: Report {
            exit_code: code,
            message: format!("result {code}")
        },
    }));
}

fn ready() -> Controller {
    let mut state = Controller::default();
    let request = state.begin_diagnostic(Action::Doctor).unwrap();
    finish(&mut state, request, 0);
    state
}

#[test]
fn startup_cannot_launch_without_readiness() {
    let mut state = Controller::default();
    assert!(!state.can_offer_launch());
    assert!(!state.offer_launch());
    assert!(state.confirm_launch().is_none());
    assert_eq!(state.phase, Phase::Idle);
}

#[test]
fn diagnostic_is_read_only_and_repeated_actions_are_ignored() {
    let mut state = Controller::default();
    let request = state.begin_diagnostic(Action::Doctor).unwrap();
    assert!(!request.allow_session_start);
    assert!(state.begin_diagnostic(Action::Doctor).is_none());
    assert!(state.begin_diagnostic(Action::Status).is_none());
    assert!(!state.offer_launch());
    assert_eq!(state.phase, Phase::Checking(Action::Doctor));
    finish(&mut state, request, 0);
    assert!(state.can_offer_launch());
}

#[test]
fn status_success_is_not_launch_readiness() {
    let mut state = ready();
    let request = state.begin_diagnostic(Action::Status).unwrap();
    assert_eq!(state.readiness, Readiness::Unknown);
    finish(&mut state, request, 0);
    assert!(!state.can_offer_launch());
}

#[test]
fn launch_cannot_be_smuggled_through_diagnostic_path() {
    let mut state = ready();
    assert!(state.begin_diagnostic(Action::Launch).is_none());
    assert_eq!(state.phase, Phase::Idle);
}

#[test]
fn cancel_does_not_emit_request_or_remember_consent() {
    let mut state = ready();
    assert!(state.offer_launch());
    assert!(state.begin_diagnostic(Action::Doctor).is_none());
    state.cancel_launch();
    assert_eq!(state.phase, Phase::Idle);
    assert!(state.confirm_launch().is_none());
    assert!(state.offer_launch());
    let request = state.confirm_launch().unwrap();
    assert_eq!(request.action, Action::Launch);
    assert!(request.allow_session_start);
    assert!(state.confirm_launch().is_none());
    assert!(!state.offer_launch());
}

#[test]
fn launch_spawn_and_exit_never_assert_rendered_readiness() {
    let mut state = ready();
    assert!(state.offer_launch());
    let request = state.confirm_launch().unwrap();
    finish(&mut state, request, 0);
    assert!(state.ui_request_active);
    assert_eq!(state.readiness, Readiness::Unknown);
    assert!(!state.can_offer_launch());
    assert!(
        state
            .request_process
            .contains("No rendered-frame verification")
    );
    state.apply(Event::UiExited {
        pid: 42,
        code: Some(0),
    });
    assert!(!state.ui_request_active);
    assert!(!state.can_offer_launch());
    assert!(state.request_process.contains("does not verify"));
    // New readiness check and explicit prompt are necessary for a second launch.
    let request = state.begin_diagnostic(Action::Doctor).unwrap();
    finish(&mut state, request, 0);
    assert!(state.confirm_launch().is_none());
    assert!(state.offer_launch());
}

#[test]
fn doctor_does_not_override_an_active_request_guard() {
    let mut state = ready();
    state.offer_launch();
    let request = state.confirm_launch().unwrap();
    finish(&mut state, request, 0);
    let request = state.begin_diagnostic(Action::Doctor).unwrap();
    finish(&mut state, request, 0);
    assert!(!state.can_offer_launch());
}

#[test]
fn failed_doctor_or_launch_cannot_retain_ready_state() {
    for code in [3, 69, 70, 75, 78] {
        let mut state = ready();
        let request = state.begin_diagnostic(Action::Doctor).unwrap();
        finish(&mut state, request, code);
        assert_eq!(state.readiness, Readiness::Unavailable);
        assert!(!state.offer_launch());
        assert_eq!(state.last_exit_code, Some(code));
        let mut state = ready();
        state.offer_launch();
        let request = state.confirm_launch().unwrap();
        finish(&mut state, request, code);
        assert!(!state.ui_request_active);
        assert!(!state.offer_launch());
    }
}

#[test]
fn stale_completion_cannot_replace_newer_request() {
    let mut state = Controller::default();
    let old = state.begin_diagnostic(Action::Doctor).unwrap();
    finish(&mut state, old, 0);
    let new = state.begin_diagnostic(Action::Status).unwrap();
    assert!(!state.apply(Event::Finished {
        request: old,
        report: Report {
            exit_code: 0,
            message: "stale success".into()
        },
    }));
    assert_eq!(state.phase, Phase::Checking(Action::Status));
    finish(&mut state, new, 70);
    assert_eq!(state.readiness, Readiness::Unavailable);
}

#[test]
fn submission_failure_recovers_busy_state_and_rejects_late_success() {
    let mut state = Controller::default();
    let request = state.begin_diagnostic(Action::Doctor).unwrap();
    state.submit_failed("disconnected");
    assert_eq!(state.phase, Phase::Idle);
    assert_eq!(state.readiness, Readiness::Unavailable);
    assert!(!state.apply(Event::Finished {
        request,
        report: Report {
            exit_code: 0,
            message: "late".into()
        },
    }));
    assert_eq!(state.message, "disconnected");
}

#[test]
fn uncertain_child_status_blocks_further_launches() {
    let mut state = ready();
    state.apply(Event::UiWaitFailed {
        pid: 5,
        message: "wait failed".into(),
    });
    assert!(state.ui_request_active);
    assert_eq!(state.readiness, Readiness::Unavailable);
    assert!(!state.can_offer_launch());
    let request = state.begin_diagnostic(Action::Doctor).unwrap();
    finish(&mut state, request, 0);
    assert!(!state.can_offer_launch());
}

#[test]
fn cancel_during_an_issued_request_cannot_claim_it_was_cancelled() {
    let mut state = ready();
    state.offer_launch();
    state.confirm_launch().unwrap();
    state.cancel_launch();
    assert_eq!(state.phase, Phase::Checking(Action::Launch));
}

#[test]
fn unsuccessful_request_exit_is_visible_as_an_error() {
    for code in [Some(1), None] {
        let mut state = ready();
        state.apply(Event::UiExited { pid: 9, code });
        assert_eq!(state.readiness, Readiness::Unavailable);
        assert!(!state.ui_request_active);
        assert!(!state.can_offer_launch());
        assert!(
            state.request_process.contains("failed")
                || state.request_process.contains("terminated")
        );
    }
}
