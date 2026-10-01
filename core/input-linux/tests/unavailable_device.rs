#![cfg(target_os = "linux")]

use rex_input_core::AndroidSize;
use rex_input_linux::{DeviceConfig, LinuxTouchDevice};
use std::{fs, io};

/// This test never grants privileges, loads modules, or emits input. It must only
/// be selected in an isolated environment where /dev/uinput is genuinely absent.
#[test]
#[ignore = "explicit missing-/dev/uinput constructor check; refuses when the path exists"]
fn real_constructor_reports_missing_uinput_without_fallback() {
    match fs::symlink_metadata("/dev/uinput") {
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        result => {
            panic!("refusing live constructor test: /dev/uinput is not proven absent: {result:?}")
        }
    }
    let config = DeviceConfig::new(AndroidSize::new(1080, 1920).unwrap(), 10).unwrap();
    let result = LinuxTouchDevice::create(config);
    assert!(matches!(result, Err(error) if error.kind() == io::ErrorKind::NotFound));
}
