use image::{ImageEncoder, codecs::png::PngEncoder};
use rex_media::{MediaConfig, MediaError, MediaSession, Serial, decode_png};
use std::time::{Duration, Instant};

#[test]
fn explicit_serial_contract_rejects_empty_options_spaces_and_controls() {
    for serial in ["", "-s", "a b", "a\nb", "$(cmd)", "a;cmd", "a/b"] {
        assert!(Serial::new(serial).is_err(), "{serial:?}");
    }
    for serial in ["emulator-5554", "R58M1234", "192.0.2.4:5555", "[::1]:5555"] {
        assert_eq!(Serial::new(serial).unwrap().as_str(), serial);
    }
    assert!(Serial::new("x".repeat(257)).is_err());
}

#[test]
fn configuration_bounds_time_and_validates_trusted_paths() {
    let mut config = MediaConfig::new(Serial::new("unit-test").unwrap());
    assert!(config.validate().is_ok());
    config.capture_interval = Duration::ZERO;
    assert!(config.validate().is_err());
    config.capture_interval = Duration::from_millis(50);
    config.capture_timeout = Duration::from_secs(31);
    assert!(config.validate().is_err());
    config.capture_timeout = Duration::from_millis(100);
    config.adb.clear();
    assert!(config.validate().is_err());
}

#[test]
fn decoder_converts_valid_rgba_to_gpui_bgra_without_changing_dimensions() {
    let mut png = Vec::new();
    PngEncoder::new(&mut png)
        .write_image(&[255, 40, 10, 255], 1, 1, image::ExtendedColorType::Rgba8)
        .unwrap();
    let decoded = decode_png(&png, 7, Instant::now()).unwrap();
    assert_eq!((decoded.width, decoded.height, decoded.number), (1, 1, 7));
    assert_eq!(decoded.bgra, [10, 40, 255, 255]);
}

#[test]
fn decoder_rejects_malformed_truncated_and_oversized_input() {
    assert!(matches!(
        decode_png(b"not png", 1, Instant::now()),
        Err(MediaError::Decode(_))
    ));
    assert!(decode_png(b"\x89PNG\r\n\x1a\n", 1, Instant::now()).is_err());
    assert_eq!(
        decode_png(&vec![0; rex_media::MAX_PNG_BYTES + 1], 1, Instant::now()).unwrap_err(),
        MediaError::OutputLimit
    );
}

#[test]
fn oversized_png_dimensions_fail_before_pixel_allocation() {
    // A valid PNG header/CRC with dimensions 9000x1 exceeds our strict decoder
    // width limit. image::PngEncoder supplies a correct header and compressed body.
    let mut png = Vec::new();
    PngEncoder::new(&mut png)
        .write_image(&vec![0; 9000 * 4], 9000, 1, image::ExtendedColorType::Rgba8)
        .unwrap();
    assert!(matches!(
        decode_png(&png, 1, Instant::now()),
        Err(MediaError::Decode(_))
    ));
}

#[test]
fn constructing_idle_session_never_contacts_configured_executables() {
    let mut config = MediaConfig::new(Serial::new("unit-test").unwrap());
    config.adb = "/not-installed/adb".into();
    config.scrcpy = "/not-installed/scrcpy".into();
    let mut session = MediaSession::new(config).unwrap();
    let handle = session.handle();
    let mut revision = 0;
    let initial = handle.take_update(&mut revision).unwrap();
    assert_eq!(initial.video, rex_media::VideoState::Stopped);
    assert_eq!(initial.audio, rex_media::AudioState::Stopped);
    assert!(initial.frame.is_none());
    session.close(Duration::from_secs(1)).unwrap();
    assert!(!handle.start_video());
    assert!(!handle.start_audio());
}
