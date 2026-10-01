#![cfg(target_os = "linux")]
use rex_launcher::{Backend, BackendError, ProcessBackend};
use std::ffi::OsStr;
use std::process::Command;
use std::time::{Duration, Instant};

#[test]
fn cli_help_is_real_process_success() {
    let result = Command::new(env!("CARGO_BIN_EXE_rex-launcher"))
        .arg("--help")
        .output()
        .unwrap();
    assert!(result.status.success());
    assert!(String::from_utf8_lossy(&result.stdout).contains("--allow-session-start"));
}

#[test]
fn cli_missing_executable_is_actionable_failure() {
    let result = Command::new(env!("CARGO_BIN_EXE_rex-launcher"))
        .args([
            "status",
            "--waydroid",
            "/nonexistent/rex-launcher-test/waydroid",
        ])
        .output()
        .unwrap();
    assert_eq!(result.status.code(), Some(69));
    // Root runners are intentionally rejected before attempting a diagnostic.
    let error = String::from_utf8_lossy(&result.stderr);
    assert!(error.contains("not found") || error.contains("non-root effective UID"));
}

#[test]
fn cli_rejects_unsupported_start_and_stop_commands() {
    for arg in ["start", "stop", "windows", "launch --allow-session-start"] {
        let result = Command::new(env!("CARGO_BIN_EXE_rex-launcher"))
            .arg(arg)
            .output()
            .unwrap();
        assert_eq!(result.status.code(), Some(64));
        assert!(String::from_utf8_lossy(&result.stderr).contains("Unsupported command"));
    }
}

#[test]
fn cli_launch_without_acknowledgement_refuses_before_missing_backend() {
    let result = Command::new(env!("CARGO_BIN_EXE_rex-launcher"))
        .args([
            "launch",
            "--waydroid",
            "/nonexistent/rex-launcher-test/waydroid",
        ])
        .output()
        .unwrap();
    assert!(matches!(result.status.code(), Some(78) | Some(69)));
    assert!(!String::from_utf8_lossy(&result.stderr).contains("not found"));
}

fn fixture(name: &str, timeout: Duration) -> Result<rex_launcher::CommandOutput, BackendError> {
    let executable = std::env::current_exe().unwrap();
    ProcessBackend.execute(
        executable.as_os_str(),
        &["--ignored", "--exact", name, "--nocapture"],
        timeout,
    )
}

#[test]
fn process_adapter_reports_missing_binary() {
    assert_eq!(
        ProcessBackend
            .execute(
                OsStr::new("/nonexistent/rex-launcher-test/waydroid"),
                &["status"],
                Duration::from_secs(1)
            )
            .unwrap_err(),
        BackendError::MissingExecutable
    );
}

#[test]
fn process_adapter_collects_stdout_and_stderr_without_shell() {
    let result = fixture("fixture_output", Duration::from_secs(2)).unwrap();
    assert!(result.success);
    assert!(String::from_utf8_lossy(&result.stdout).contains("fixture stdout"));
    assert!(String::from_utf8_lossy(&result.stderr).contains("fixture stderr"));
}

#[test]
fn process_adapter_preserves_nonzero_exit() {
    let result = fixture("fixture_failure", Duration::from_secs(2)).unwrap();
    assert!(!result.success);
    assert_eq!(result.code, Some(17));
}

#[test]
fn process_adapter_deadline_is_bounded() {
    let start = Instant::now();
    assert_eq!(
        fixture("fixture_sleep", Duration::from_millis(100)).unwrap_err(),
        BackendError::Timeout
    );
    assert!(start.elapsed() < Duration::from_secs(2));
}

#[test]
fn process_adapter_output_limit_is_bounded() {
    let start = Instant::now();
    assert_eq!(
        fixture("fixture_large", Duration::from_secs(2)).unwrap_err(),
        BackendError::OutputLimit
    );
    assert!(start.elapsed() < Duration::from_secs(2));
}

// Ignored subprocess fixtures are invoked explicitly by the parent tests above.
// They do not invoke Waydroid, shells, services, or privileged tools.
#[test]
#[ignore]
fn fixture_output() {
    println!("fixture stdout");
    eprintln!("fixture stderr");
}
#[test]
#[ignore]
fn fixture_failure() {
    std::process::exit(17);
}
#[test]
#[ignore]
fn fixture_sleep() {
    std::thread::sleep(Duration::from_secs(30));
}
#[test]
#[ignore]
fn fixture_large() {
    println!("{}", "x".repeat(128 * 1024));
}
