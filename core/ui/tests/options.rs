use rex_ui::options::UiOptions;
fn parse(args: &[&str]) -> Result<Option<UiOptions>, String> {
    UiOptions::parse(args.iter().map(Into::into))
}
#[test]
fn explicit_serial_is_not_a_launch_or_audio_permission() {
    let options = parse(&["--adb-serial", "emulator-5554"]).unwrap().unwrap();
    assert_eq!(options.media.unwrap().serial.as_str(), "emulator-5554");
    assert!(!options.launcher.allow_session_start);
}
#[test]
fn media_options_do_not_select_an_ambient_or_default_device() {
    for args in [
        vec!["--adb", "adb"],
        vec!["--scrcpy", "scrcpy"],
        vec!["--keymap", "test.json"],
    ] {
        assert!(parse(&args).unwrap_err().contains("--adb-serial"));
    }
}
#[test]
fn timing_duplicates_and_pregrants_are_rejected() {
    for args in [
        vec!["--adb-serial", "a", "--adb-serial", "b"],
        vec!["--adb-serial", "a", "--capture-interval-ms", "0"],
        vec!["--adb-serial", "a", "--capture-timeout-ms", "abc"],
        vec!["--allow-session-start"],
        vec!["--enable-input"],
        vec!["--start-audio"],
    ] {
        assert!(parse(&args).is_err(), "{args:?}");
    }
}
#[test]
fn existing_launcher_configuration_is_still_delegated() {
    let options = parse(&["--waydroid", "/usr/bin/waydroid", "--timeout-ms", "500"])
        .unwrap()
        .unwrap();
    assert!(options.media.is_none());
    assert_eq!(options.launcher.timeout.as_millis(), 500);
    assert_eq!(options.launcher.executable, "/usr/bin/waydroid");
}

#[test]
fn streaming_backend_requires_explicit_selection_and_bounded_stall_timeout() {
    let options = parse(&[
        "--adb-serial",
        "device123",
        "--video-backend",
        "screenrecord",
        "--ffmpeg",
        "/usr/bin/ffmpeg",
        "--stream-stall-ms",
        "750",
    ])
    .unwrap()
    .unwrap();
    let config = options.media.unwrap();
    assert_eq!(config.video_backend, rex_media::VideoBackend::Screenrecord);
    assert_eq!(config.stream_stall_timeout.as_millis(), 750);
    for args in [
        vec!["--video-backend", "screenrecord"],
        vec!["--adb-serial", "a", "--video-backend", "auto"],
        vec!["--adb-serial", "a", "--stream-stall-ms", "499"],
    ] {
        assert!(parse(&args).is_err());
    }
}
