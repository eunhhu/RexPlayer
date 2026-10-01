use rex_launcher::{
    run, Action, Backend, BackendError, CommandOutput, HostContext, Options, RuntimeStatus, State,
};
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;
use std::time::Duration;

const RUNNING: &str = "Session:\tRUNNING\nContainer:\tRUNNING\nVendor type:\tMAINLINE\nIP address:\t192.0.2.1\nSession user:\talice(1000)\nWayland display:\twayland-0\n";

struct FakeBackend {
    output: Result<CommandOutput, BackendError>,
    calls: Vec<Vec<String>>,
    ui_result: Result<u32, BackendError>,
}

impl FakeBackend {
    fn status(text: &str) -> Self {
        Self {
            output: Ok(CommandOutput {
                success: true,
                code: Some(0),
                stdout: text.as_bytes().to_vec(),
                stderr: vec![],
            }),
            calls: vec![],
            ui_result: Ok(4242),
        }
    }
}

impl Backend for FakeBackend {
    fn execute(
        &mut self,
        executable: &OsStr,
        args: &[&str],
        _timeout: Duration,
    ) -> Result<CommandOutput, BackendError> {
        assert_eq!(executable, OsStr::new("/trusted/waydroid"));
        self.calls
            .push(args.iter().map(|s| s.to_string()).collect());
        std::mem::replace(
            &mut self.output,
            Err(BackendError::Io("Unexpected second diagnostic".into())),
        )
    }
    fn spawn_ui(&mut self, executable: &OsStr) -> Result<u32, BackendError> {
        assert_eq!(executable, OsStr::new("/trusted/waydroid"));
        self.calls.push(vec!["show-full-ui".into()]);
        self.ui_result.clone()
    }
}

fn host() -> HostContext {
    HostContext {
        os: "linux".into(),
        arch: "x86_64".into(),
        effective_uid: Some(1000),
        wayland_display: Some("wayland-0".into()),
        runtime_dir: Some(PathBuf::from("/run/user/1000")),
    }
}
fn options(action: Action) -> Options {
    Options {
        action,
        executable: "/trusted/waydroid".into(),
        timeout: Duration::from_secs(1),
        allow_session_start: false,
    }
}
fn parse(args: &[&str]) -> Result<Option<Options>, String> {
    Options::parse(args.iter().map(OsString::from))
}

#[test]
fn parses_documented_running_status() {
    let status = RuntimeStatus::parse(RUNNING.as_bytes()).unwrap();
    assert_eq!(status.session, State::Running);
    assert_eq!(status.container, Some(State::Running));
    assert_eq!(status.session_uid, Some(1000));
}

#[test]
fn parses_upstream_stopped_status_without_container() {
    let status = RuntimeStatus::parse(b"Session:\tSTOPPED\nVendor type:\tMAINLINE\n").unwrap();
    assert_eq!(status.session, State::Stopped);
    assert_eq!(status.container, None);
}

#[test]
fn rejects_incomplete_conflicting_or_unknown_output() {
    for text in [
        "",
        "Waydroid is not initialized, run waydroid init",
        "Session: RUNNING",
        "Session: STARTING",
        "Session: STOPPED\nSession: RUNNING",
        "Session: STOPPED\nContainer: RUNNING",
        "Session: STOPPED\nUnknown: yes",
        "Session: STOPPED\n\u{1b}[0m",
    ] {
        assert!(
            RuntimeStatus::parse(text.as_bytes()).is_err(),
            "accepted {text:?}"
        );
    }
    assert!(RuntimeStatus::parse(&[0xff, 0xfe]).is_err());
    assert!(RuntimeStatus::parse(RUNNING.replace("alice(1000)", "1000").as_bytes()).is_err());
    assert!(RuntimeStatus::parse(format!("{RUNNING}Container: RUNNING\n").as_bytes()).is_err());
}

#[test]
fn doctor_only_reads_status_and_does_not_claim_rendering() {
    let mut fake = FakeBackend::status(RUNNING);
    let report = run(&options(Action::Doctor), &host(), &mut fake);
    assert_eq!(report.exit_code, 0);
    assert!(report.message.contains("rendered frames are unverified"));
    assert_eq!(fake.calls, vec![vec!["status"]]);
}

#[test]
fn status_remains_read_only_when_stopped_or_headless() {
    let mut fake = FakeBackend::status("Session: STOPPED\nVendor type: MAINLINE\n");
    let mut context = host();
    context.wayland_display = None;
    let report = run(&options(Action::Status), &context, &mut fake);
    assert_eq!(report.exit_code, 0);
    assert!(report.message.contains("Stopped"));
    assert_eq!(fake.calls, vec![vec!["status"]]);
}

#[test]
fn launch_without_acknowledgement_executes_nothing() {
    let mut fake = FakeBackend::status(RUNNING);
    let report = run(&options(Action::Launch), &host(), &mut fake);
    assert_eq!(report.exit_code, 78);
    assert!(report.message.contains("--allow-session-start"));
    assert!(fake.calls.is_empty());
}

#[test]
fn acknowledged_launch_checks_then_spawns_without_claiming_a_frame() {
    let mut fake = FakeBackend::status(RUNNING);
    let mut opts = options(Action::Launch);
    opts.allow_session_start = true;
    let report = run(&opts, &host(), &mut fake);
    assert_eq!(report.exit_code, 0);
    assert!(report.message.contains("PID 4242"));
    assert!(report
        .message
        .contains("Command completion and rendered frames are unverified"));
    assert_eq!(fake.calls, vec![vec!["status"], vec!["show-full-ui"]]);
}

#[test]
fn launch_rejects_stopped_frozen_foreign_user_or_display() {
    let mut opts = options(Action::Launch);
    opts.allow_session_start = true;
    for text in [
        "Session: STOPPED\nVendor type: MAINLINE\n".into(),
        RUNNING.replace("Container:\tRUNNING", "Container:\tFROZEN"),
        RUNNING.replace("alice(1000)", "other(1001)"),
        RUNNING.replace("wayland-0", "wayland-1"),
    ] {
        let mut fake = FakeBackend::status(&text);
        assert_eq!(run(&opts, &host(), &mut fake).exit_code, 3);
        assert_eq!(fake.calls, vec![vec!["status"]]);
    }
}

#[test]
fn launch_rejects_absent_display_and_invalid_runtime_directory() {
    let mut opts = options(Action::Launch);
    opts.allow_session_start = true;
    for (display, runtime) in [
        (None, Some(PathBuf::from("/run/user/1000"))),
        (Some("wayland-0".into()), None),
        (Some("wayland-0".into()), Some(PathBuf::from("relative"))),
    ] {
        let mut context = host();
        context.wayland_display = display;
        context.runtime_dir = runtime;
        let mut fake = FakeBackend::status(RUNNING);
        assert_eq!(run(&opts, &context, &mut fake).exit_code, 3);
        assert_eq!(fake.calls, vec![vec!["status"]]);
    }
}

#[test]
fn unsupported_platform_and_privileged_context_never_execute() {
    for (os, uid) in [
        ("windows", Some(1000)),
        ("macos", Some(1000)),
        ("linux", Some(0)),
        ("linux", None),
    ] {
        let mut context = host();
        context.os = os.into();
        context.effective_uid = uid;
        let mut fake = FakeBackend::status(RUNNING);
        assert_eq!(
            run(&options(Action::Status), &context, &mut fake).exit_code,
            69
        );
        assert!(fake.calls.is_empty());
    }
}

#[test]
fn diagnostic_errors_fail_closed() {
    for (error, code) in [
        (BackendError::MissingExecutable, 69),
        (BackendError::Timeout, 75),
        (BackendError::OutputLimit, 70),
        (BackendError::Io("permission denied".into()), 70),
    ] {
        let mut fake = FakeBackend::status(RUNNING);
        fake.output = Err(error);
        assert_eq!(
            run(&options(Action::Doctor), &host(), &mut fake).exit_code,
            code
        );
        assert_eq!(fake.calls, vec![vec!["status"]]);
    }
}

#[test]
fn nonzero_exit_and_stderr_fail_closed_even_with_running_stdout() {
    for (success, stderr) in [(false, vec![]), (true, b"warning".to_vec())] {
        let mut fake = FakeBackend::status(RUNNING);
        fake.output = Ok(CommandOutput {
            success,
            code: Some(if success { 0 } else { 1 }),
            stdout: RUNNING.as_bytes().to_vec(),
            stderr,
        });
        let mut opts = options(Action::Launch);
        opts.allow_session_start = true;
        assert_eq!(run(&opts, &host(), &mut fake).exit_code, 70);
        assert_eq!(fake.calls, vec![vec!["status"]]);
    }
}

#[test]
fn ui_spawn_error_is_not_success() {
    let mut fake = FakeBackend::status(RUNNING);
    fake.ui_result = Err(BackendError::MissingExecutable);
    let mut opts = options(Action::Launch);
    opts.allow_session_start = true;
    assert_eq!(run(&opts, &host(), &mut fake).exit_code, 70);
}

#[test]
fn parses_options_and_rejects_ambiguous_or_unsupported_commands() {
    let opts = parse(&[
        "launch",
        "--allow-session-start",
        "--waydroid",
        "/trusted/waydroid",
        "--timeout-ms",
        "500",
    ])
    .unwrap()
    .unwrap();
    assert!(opts.allow_session_start);
    assert_eq!(opts.timeout, Duration::from_millis(500));
    assert!(parse(&["--help"]).unwrap().is_none());
    for args in [
        vec![],
        vec!["start"],
        vec!["stop"],
        vec!["doctor", "--allow-session-start"],
        vec!["status", "--timeout-ms", "99"],
        vec!["status", "--timeout-ms", "30001"],
        vec!["status", "--timeout-ms", "oops"],
        vec!["status", "--waydroid"],
        vec!["status", "--waydroid", ""],
        vec!["status", "--waydroid", "--help"],
        vec!["status", "--timeout-ms", "100", "--timeout-ms", "100"],
        vec!["launch", "--allow-session-start", "--allow-session-start"],
        vec!["--help", "status"],
    ] {
        assert!(parse(&args).is_err(), "accepted {args:?}");
    }
}

#[test]
fn unsupported_linux_arch_never_executes() {
    let mut context = host();
    context.arch = "sparc64".into();
    let mut fake = FakeBackend::status(RUNNING);
    assert_eq!(
        run(&options(Action::Status), &context, &mut fake).exit_code,
        69
    );
    assert!(fake.calls.is_empty());
}
