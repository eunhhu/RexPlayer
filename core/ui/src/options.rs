//! Validated startup configuration. Selecting a serial never starts media.
use rex_launcher::Options;
use rex_media::{MediaConfig, Serial, VideoBackend};
use std::{ffi::OsString, path::PathBuf, time::Duration};

pub const HELP: &str = "RexPlayer native integrated player (Linux x86_64/aarch64)\n\
Usage: rex-player [--waydroid TRUSTED_PATH] [--timeout-ms 100..30000]\n\
       rex-player --adb-serial SERIAL [--adb TRUSTED_PATH] [--scrcpy TRUSTED_PATH]\n\
                  [--capture-interval-ms 50..5000] [--capture-timeout-ms 100..30000]\n\
                  [--video-backend screenshot|screenrecord] [--ffmpeg TRUSTED_PATH]\n\
                  [--stream-stall-ms 500..30000] [--keymap PROFILE.json]\n\
Selected ADB devices must already be connected and authorized. No auto-selection,\n\
network discovery, pairing, TCP/IP setup, or provisioning is performed.\n\
Choose Start capture to show decoded Android frames inside RexPlayer.\n\
Default PNG polling is 5 FPS maximum. Optional H264/FFmpeg streaming is CPU-copy;\n\
performance and AV sync are unverified. Screenrecord may end at its device time limit.\n\
Audio and Linux virtual touchscreen input each require explicit in-window consent.\n\
Every Android UI request requires confirmation in the window.\n\
Keyboard: Escape releases/disables input and cancels a shown prompt; Ctrl-Q closes.\n\
Ctrl-R Doctor, Ctrl-S Status, Ctrl-L request Waydroid launch, Ctrl-Enter confirm launch.\n\
Without --adb-serial the shell remains a Waydroid diagnostic/launcher workspace.";

#[derive(Debug)]
pub struct UiOptions {
    pub launcher: Options,
    pub media: Option<MediaConfig>,
    pub keymap: Option<PathBuf>,
}
impl UiOptions {
    pub fn parse(args: impl IntoIterator<Item = OsString>) -> Result<Option<Self>, String> {
        let args: Vec<_> = args.into_iter().collect();
        if args.len() == 1 && (args[0] == "--help" || args[0] == "-h") {
            return Ok(None);
        }
        let mut launcher_args = vec![OsString::from("doctor")];
        let (
            mut serial,
            mut adb,
            mut scrcpy,
            mut interval,
            mut timeout,
            mut keymap,
            mut backend,
            mut ffmpeg,
            mut stall,
        ) = (None, None, None, None, None, None, None, None, None);
        let mut args = args.into_iter();
        while let Some(arg) = args.next() {
            let destination = match arg.to_str() {
                Some("--adb-serial") => &mut serial,
                Some("--adb") => &mut adb,
                Some("--scrcpy") => &mut scrcpy,
                Some("--capture-interval-ms") => &mut interval,
                Some("--capture-timeout-ms") => &mut timeout,
                Some("--keymap") => &mut keymap,
                Some("--video-backend") => &mut backend,
                Some("--ffmpeg") => &mut ffmpeg,
                Some("--stream-stall-ms") => &mut stall,
                Some("--waydroid" | "--timeout-ms") => {
                    launcher_args.push(arg);
                    launcher_args.push(args.next().ok_or("Launcher option is missing its value")?);
                    continue;
                }
                _ => {
                    launcher_args.push(arg);
                    continue;
                }
            };
            if destination.is_some() {
                return Err("Repeated media/keymap option".into());
            }
            let value = args
                .next()
                .ok_or("Media/keymap option is missing its value")?;
            if value.is_empty() || value.to_string_lossy().starts_with('-') {
                return Err(
                    "Media/keymap option requires a nonempty value, not another flag".into(),
                );
            }
            *destination = Some(value);
        }
        let launcher = Options::parse(launcher_args)?.ok_or("Expected launcher configuration")?;
        let media = if let Some(serial) = serial {
            let serial = Serial::new(
                serial
                    .into_string()
                    .map_err(|_| "ADB serial must be valid UTF-8")?,
            )
            .map_err(|e| e.to_string())?;
            let mut config = MediaConfig::new(serial);
            if let Some(adb) = adb {
                config.adb = adb;
            }
            if let Some(scrcpy) = scrcpy {
                config.scrcpy = scrcpy;
            }
            if let Some(ffmpeg) = ffmpeg {
                config.ffmpeg = ffmpeg;
            }
            if let Some(backend) = backend {
                config.video_backend = match backend.to_str() {
                    Some("screenshot") => VideoBackend::Screenshot,
                    Some("screenrecord") => VideoBackend::Screenrecord,
                    _ => return Err("--video-backend must be screenshot or screenrecord".into()),
                };
            }
            if let Some(value) = stall {
                config.stream_stall_timeout = milliseconds(value)?;
            }
            if let Some(value) = interval {
                config.capture_interval = milliseconds(value)?;
            }
            if let Some(value) = timeout {
                config.capture_timeout = milliseconds(value)?;
            }
            config.validate().map_err(|e| e.to_string())?;
            Some(config)
        } else {
            if adb.is_some()
                || scrcpy.is_some()
                || interval.is_some()
                || timeout.is_some()
                || keymap.is_some()
                || backend.is_some()
                || ffmpeg.is_some()
                || stall.is_some()
            {
                return Err("--adb-serial is required with media/keymap options; no device is auto-selected".into());
            }
            None
        };
        Ok(Some(Self {
            launcher,
            media,
            keymap: keymap.map(PathBuf::from),
        }))
    }
}
fn milliseconds(value: OsString) -> Result<Duration, String> {
    let value = value
        .to_str()
        .and_then(|v| v.parse::<u64>().ok())
        .ok_or("Media timing value must be an integer in milliseconds")?;
    Ok(Duration::from_millis(value))
}
