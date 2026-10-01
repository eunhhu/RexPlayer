#![cfg(all(target_os = "linux", feature = "test-fixtures"))]
use rex_media::{AudioState, MediaConfig, MediaHandle, MediaSession, Serial, Update, VideoState};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

fn session(case: &str) -> (MediaSession, PathBuf) {
    static NEXT: AtomicU64 = AtomicU64::new(1);
    let serial = format!(
        "fixture-{case}-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    );
    let path = std::env::temp_dir().join(format!("rex-media-{serial}.pid"));
    let mut config = MediaConfig::new(Serial::new(serial).unwrap());
    config.adb = env!("CARGO_BIN_EXE_rex-media-fixture").into();
    config.scrcpy = config.adb.clone();
    config.capture_timeout = if case == "flood" {
        Duration::from_secs(2)
    } else {
        Duration::from_millis(150)
    };
    config.capture_interval = Duration::from_millis(50);
    (MediaSession::new(config).unwrap(), path)
}
fn await_update(handle: &MediaHandle, predicate: impl Fn(&Update) -> bool) -> Update {
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut revision = 0;
    loop {
        if let Some(update) = handle.take_update(&mut revision)
            && predicate(&update)
        {
            return update;
        }
        assert!(Instant::now() < deadline, "media update deadline exceeded");
        std::thread::sleep(Duration::from_millis(5));
    }
}
fn pid(path: &PathBuf) -> u32 {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if let Ok(text) = std::fs::read_to_string(path)
            && let Ok(pid) = text.parse()
        {
            return pid;
        }
        assert!(Instant::now() < deadline, "fixture PID not written");
        std::thread::sleep(Duration::from_millis(5));
    }
}
fn assert_reaped(pid: u32) {
    let deadline = Instant::now() + Duration::from_secs(2);
    while std::path::Path::new(&format!("/proc/{pid}")).exists() {
        assert!(
            Instant::now() < deadline,
            "owned fixture process not reaped"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn actual_child_png_reaches_latest_frame_mailbox_and_restarts() {
    let (mut session, path) = session("png");
    let handle = session.handle();
    assert!(handle.start_video());
    assert!(!handle.start_video());
    let first = await_update(&handle, |u| u.frame.is_some());
    let first = first.frame.unwrap();
    assert_eq!((first.width, first.height), (2, 1));
    assert_eq!(&first.bgra[..4], [16, 0, 255, 255]);
    handle.stop_video();
    await_update(&handle, |u| u.video == VideoState::Stopped);
    assert!(handle.start_video());
    let next = await_update(&handle, |u| u.frame.is_some()).frame.unwrap();
    assert!(next.number > first.number);
    session.close(Duration::from_secs(2)).unwrap();
    assert_reaped(pid(&path));
    let _ = std::fs::remove_file(path);
}

#[test]
fn malformed_nonzero_timeout_and_output_limits_fail_closed_and_reap() {
    for (case, expected) in [
        ("malformed", "not a PNG"),
        ("nonzero", "unauthorized"),
        ("timeout", "timed out"),
        ("flood", "limit"),
        ("stderr", "limit"),
        ("inherited", "timed out"),
    ] {
        let (mut session, path) = session(case);
        let handle = session.handle();
        handle.start_video();
        let update = await_update(&handle, |u| matches!(u.video, VideoState::Failed(_)));
        let VideoState::Failed(error) = update.video else {
            unreachable!()
        };
        assert!(error.contains(expected), "{case}: {error}");
        assert!(update.frame.is_none());
        session.close(Duration::from_secs(2)).unwrap();
        assert_reaped(pid(&path));
        let _ = std::fs::remove_file(path);
    }
}

#[test]
fn stop_interrupts_inflight_screenshot_and_reaps_without_waiting_for_timeout() {
    let (mut session, path) = session("timeout");
    let handle = session.handle();
    handle.start_video();
    let process = pid(&path);
    let started = Instant::now();
    handle.stop_video();
    session.close(Duration::from_secs(1)).unwrap();
    assert!(started.elapsed() < Duration::from_secs(1));
    assert_reaped(process);
    let _ = std::fs::remove_file(path);
}

#[test]
fn audio_only_command_owns_one_process_and_stop_reaps_it() {
    let (mut session, path) = session("audio");
    let handle = session.handle();
    assert!(handle.start_audio());
    assert!(!handle.start_audio());
    let update = await_update(&handle, |u| {
        matches!(u.audio, AudioState::ProcessRunning { .. })
    });
    let AudioState::ProcessRunning { pid } = update.audio else {
        unreachable!()
    };
    handle.stop_audio();
    session.close(Duration::from_secs(2)).unwrap();
    assert_reaped(pid);
    let _ = std::fs::remove_file(path);
}

#[test]
fn audio_process_failure_is_not_playback_success() {
    let (mut session, path) = session("audiofail");
    let handle = session.handle();
    handle.start_audio();
    let update = await_update(&handle, |u| matches!(u.audio, AudioState::Failed(_)));
    let AudioState::Failed(error) = update.audio else {
        unreachable!()
    };
    assert!(error.contains("7"));
    session.close(Duration::from_secs(2)).unwrap();
    assert_reaped(pid(&path));
    let _ = std::fs::remove_file(path);
}

#[test]
fn dropping_owner_cancels_owned_media_children() {
    let (session, path) = session("timeout");
    let handle = session.handle();
    handle.start_video();
    let process = pid(&path);
    drop(session);
    assert_reaped(process);
    assert!(!handle.start_video());
    let _ = std::fs::remove_file(path);
}
