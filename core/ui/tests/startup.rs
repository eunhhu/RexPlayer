#![cfg(feature = "native-gpui")]

use std::process::{Command, Output};

fn invoke(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_rex-player"))
        .args(args)
        .env_remove("DISPLAY")
        .env_remove("WAYLAND_DISPLAY")
        .output()
        .expect("built native binary must execute")
}

#[test]
fn help_does_not_initialize_gpui_or_require_display() {
    let output = invoke(&["--help"]);
    assert!(output.status.success());
    assert!(
        String::from_utf8_lossy(&output.stdout)
            .contains("Every Android UI request requires confirmation")
    );
    assert!(output.stderr.is_empty());
}

#[test]
fn no_display_fails_explicitly_without_gpui_panic() {
    let output = invoke(&[]);
    assert_eq!(output.status.code(), Some(69));
    assert!(String::from_utf8_lossy(&output.stderr).contains("No graphical display found"));
    assert!(!String::from_utf8_lossy(&output.stderr).contains("panicked"));
}

#[test]
fn empty_display_variables_also_fail_closed() {
    let output = Command::new(env!("CARGO_BIN_EXE_rex-player"))
        .env("DISPLAY", "")
        .env("WAYLAND_DISPLAY", "")
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(69));
    assert!(String::from_utf8_lossy(&output.stderr).contains("No graphical display found"));
}

#[test]
fn acknowledgement_cannot_be_pregranted_on_command_line() {
    let output = invoke(&["--allow-session-start"]);
    assert_eq!(output.status.code(), Some(64));
    assert!(String::from_utf8_lossy(&output.stderr).contains("command-inapplicable option"));
}

#[test]
fn launcher_option_validation_is_reused_before_opening_a_window() {
    let output = invoke(&["--timeout-ms", "1"]);
    assert_eq!(output.status.code(), Some(64));
    assert!(String::from_utf8_lossy(&output.stderr).contains("100 to 30000"));
}
