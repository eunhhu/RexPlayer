//! Harmless subprocess fixture. Only built with the explicit test-fixtures feature.
use image::{ImageEncoder, codecs::png::PngEncoder};
use std::{
    io::{self, Write},
    time::Duration,
};

fn main() {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("--fixture-descendant") {
        std::thread::sleep(Duration::from_millis(650));
        return;
    }
    let (serial, audio) = if args.first().map(String::as_str) == Some("-s") {
        assert_eq!(args.len(), 5);
        assert_eq!(&args[2..], ["exec-out", "screencap", "-p"]);
        (args[1].as_str(), false)
    } else {
        assert_eq!(args.len(), 7);
        assert_eq!(
            &args[1..],
            [
                "--no-video",
                "--no-control",
                "--no-window",
                "--audio-source=output",
                "--require-audio",
                "--no-clipboard-autosync"
            ]
        );
        (
            args[0].strip_prefix("--serial=").expect("explicit serial"),
            true,
        )
    };
    assert!(serial.starts_with("fixture-"));
    assert_eq!(std::env::var("ADB_MDNS_AUTO_CONNECT").as_deref(), Ok("0"));
    for key in [
        "ANDROID_SERIAL",
        "ADB_SERVER_SOCKET",
        "ANDROID_ADB_SERVER_ADDRESS",
        "ANDROID_ADB_SERVER_PORT",
        "ADB_SERVER_HOST",
        "ADB_SERVER_PORT",
    ] {
        assert!(std::env::var_os(key).is_none(), "ambient routing {key}");
    }
    let path = std::env::temp_dir().join(format!("rex-media-{serial}.pid"));
    std::fs::write(path, std::process::id().to_string()).unwrap();
    if audio {
        eprintln!("fixture audio process started; this is not audio playback");
        if serial.contains("audiofail") {
            std::process::exit(7);
        }
        loop {
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    if serial.contains("timeout") {
        loop {
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    if serial.contains("flood") {
        loop {
            io::stdout().write_all(&[0_u8; 65536]).unwrap();
        }
    }
    if serial.contains("stderr") {
        io::stderr().write_all(&[b'e'; 65537]).unwrap();
        std::thread::sleep(Duration::from_secs(2));
        return;
    }
    if serial.contains("nonzero") {
        eprintln!("selected device unauthorized");
        std::process::exit(9);
    }
    if serial.contains("malformed") {
        print!("not a PNG");
        return;
    }
    if serial.contains("inherited") {
        let mut child = std::process::Command::new(std::env::current_exe().unwrap())
            .arg("--fixture-descendant")
            .spawn()
            .unwrap();
        // This fixture specifically holds inherited descriptors after the direct
        // parent exits. A separate short-lived reaper avoids global process kills.
        std::thread::spawn(move || {
            let _ = child.wait();
        });
        return;
    }
    let bytes = [255_u8, 0, 16, 255, 0, 255, 32, 255];
    PngEncoder::new(io::stdout())
        .write_image(&bytes, 2, 1, image::ExtendedColorType::Rgba8)
        .unwrap();
}
