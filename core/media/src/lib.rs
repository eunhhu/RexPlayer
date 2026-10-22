//! Compatibility media for one explicitly selected, already authorized ADB device.
//!
//! Screenshot polling is CPU-copy based, with no real-time/zero-copy guarantee.
//! Audio delegates decoding/playback to a separately installed trusted scrcpy.
//! Nothing discovers/connects devices, provisions Android, or changes host settings.

use image::{ImageDecoder, ImageFormat, ImageReader, Limits};
use std::ffi::OsString;
use std::fmt;
use std::io::Cursor;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[cfg(target_os = "linux")]
mod process;
#[cfg(target_os = "linux")]
mod stream;

pub const MAX_PNG_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_PIXELS: u64 = 16 * 1024 * 1024;
pub const MAX_DIMENSION: u32 = 8192;
pub const AUDIO_NOTICE: &str = "Start scrcpy device-output audio for this selected device. Android 11+ is required. scrcpy uploads/runs its temporary server and may mute device-speaker output while forwarding audio to this computer. Microphone capture and remote input control are disabled. Audible playback is not verified from process startup.";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Serial(String);
impl Serial {
    pub fn new(value: impl Into<String>) -> Result<Self, MediaError> {
        let value = value.into();
        if value.is_empty()
            || value.len() > 256
            || value.starts_with('-')
            || !value
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"._:-[]".contains(&b))
        {
            return Err(MediaError::Configuration("ADB serial must be explicit, 1..256 ASCII letters/digits or . _ : - [ ], and cannot start with '-'".into()));
        }
        Ok(Self(value))
    }
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoBackend {
    Screenshot,
    Screenrecord,
}

#[derive(Clone, Debug)]
pub struct MediaConfig {
    pub serial: Serial,
    pub adb: OsString,
    pub scrcpy: OsString,
    pub ffmpeg: OsString,
    pub video_backend: VideoBackend,
    pub stream_stall_timeout: Duration,
    pub capture_interval: Duration,
    pub capture_timeout: Duration,
}
impl MediaConfig {
    pub fn new(serial: Serial) -> Self {
        Self {
            serial,
            adb: "adb".into(),
            scrcpy: "scrcpy".into(),
            ffmpeg: "ffmpeg".into(),
            video_backend: VideoBackend::Screenshot,
            stream_stall_timeout: Duration::from_secs(5),
            capture_interval: Duration::from_millis(200),
            capture_timeout: Duration::from_secs(3),
        }
    }
    pub fn validate(&self) -> Result<(), MediaError> {
        if self.adb.is_empty() || self.scrcpy.is_empty() || self.ffmpeg.is_empty() {
            return Err(MediaError::Configuration(
                "Trusted adb/scrcpy executable paths cannot be empty".into(),
            ));
        }
        if !(Duration::from_millis(50)..=Duration::from_secs(5)).contains(&self.capture_interval)
            || !(Duration::from_millis(100)..=Duration::from_secs(30))
                .contains(&self.capture_timeout)
        {
            return Err(MediaError::Configuration(
                "Capture interval must be 50..5000 ms and timeout 100..30000 ms".into(),
            ));
        }
        if !(Duration::from_millis(500)..=Duration::from_secs(30))
            .contains(&self.stream_stall_timeout)
        {
            return Err(MediaError::Configuration(
                "Stream stall timeout must be 500..30000 ms".into(),
            ));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MediaError {
    Configuration(String),
    Unsupported,
    Cancelled,
    Timeout,
    OutputLimit,
    Process(String),
    Decode(String),
    Cleanup(String),
}
impl fmt::Display for MediaError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Configuration(s) | Self::Process(s) | Self::Decode(s) | Self::Cleanup(s) => {
                f.write_str(s)
            }
            Self::Unsupported => {
                f.write_str("Media processes are currently supported on Linux only")
            }
            Self::Cancelled => f.write_str("Capture cancelled"),
            Self::Timeout => f.write_str(
                "ADB screenshot timed out; check this selected device's authorization/connection",
            ),
            Self::OutputLimit => {
                f.write_str("Screenshot or diagnostic output exceeded its safety limit")
            }
        }
    }
}
impl std::error::Error for MediaError {}

/// Owned decoded BGRA bytes, ready to move into GPUI without image decoding on its UI thread.
#[derive(Debug)]
pub struct Frame {
    pub number: u64,
    pub width: u32,
    pub height: u32,
    pub bgra: Vec<u8>,
    pub captured_at: Instant,
    pub capture_duration: Duration,
}

pub fn decode_png(bytes: &[u8], number: u64, started: Instant) -> Result<Frame, MediaError> {
    if bytes.len() > MAX_PNG_BYTES {
        return Err(MediaError::OutputLimit);
    }
    if !bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Err(MediaError::Decode(
            "ADB screenshot is not a PNG image".into(),
        ));
    }
    let mut reader = ImageReader::with_format(Cursor::new(bytes), ImageFormat::Png);
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_DIMENSION);
    limits.max_image_height = Some(MAX_DIMENSION);
    limits.max_alloc = Some(MAX_PIXELS * 8);
    reader.limits(limits);
    let decoder = reader
        .into_decoder()
        .map_err(|e| MediaError::Decode(format!("Invalid screenshot PNG: {e}")))?;
    let (width, height) = decoder.dimensions();
    if width == 0 || height == 0 || u64::from(width) * u64::from(height) > MAX_PIXELS {
        return Err(MediaError::Decode(
            "Screenshot dimensions exceed the pixel limit".into(),
        ));
    }
    let mut image = image::DynamicImage::from_decoder(decoder)
        .map_err(|e| MediaError::Decode(format!("Could not decode screenshot PNG: {e}")))?
        .into_rgba8();
    for pixel in image.as_chunks_mut::<4>().0 {
        pixel.swap(0, 2);
    }
    Ok(Frame {
        number,
        width,
        height,
        bgra: image.into_raw(),
        captured_at: Instant::now(),
        capture_duration: started.elapsed(),
    })
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum VideoState {
    Stopped,
    Stopping,
    Starting,
    Capturing { frames: u64 },
    Failed(String),
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AudioState {
    Stopped,
    Stopping,
    Starting,
    ProcessRunning { pid: u32 },
    Failed(String),
}
#[derive(Debug)]
pub struct Update {
    pub revision: u64,
    pub video: VideoState,
    pub audio: AudioState,
    pub audio_diagnostics: String,
    pub frame: Option<Frame>,
}

struct Shared {
    config: MediaConfig,
    video: AtomicBool,
    audio: AtomicBool,
    video_busy: AtomicBool,
    audio_busy: AtomicBool,
    closing: AtomicBool,
    generation: AtomicU64,
    audio_generation: AtomicU64,
    poisoned: AtomicBool,
    update: Mutex<Update>,
}
impl Shared {
    fn change(&self, change: impl FnOnce(&mut Update)) {
        if let Ok(mut state) = self.update.lock() {
            change(&mut state);
            state.revision = state.revision.wrapping_add(1);
        }
    }
    fn video_idle(&self) {
        self.video_busy.store(false, Ordering::Release);
        self.change(|state| {
            if !self.video.load(Ordering::Acquire) && state.video == VideoState::Stopping {
                state.video = VideoState::Stopped;
            }
        });
    }
    #[cfg(target_os = "linux")]
    fn audio_idle(&self) {
        self.audio_busy.store(false, Ordering::Release);
        self.change(|state| {
            if !self.audio.load(Ordering::Acquire) && state.audio == AudioState::Stopping {
                state.audio = AudioState::Stopped;
            }
        });
    }
    fn poison(&self, message: String) {
        // Terminal fail-closed latch: pending restart requests cannot spawn more
        // children after an earlier owned process failed to clean up.
        self.poisoned.store(true, Ordering::Release);
        self.closing.store(true, Ordering::Release);
        self.video.store(false, Ordering::Release);
        self.audio.store(false, Ordering::Release);
        self.generation.fetch_add(1, Ordering::AcqRel);
        self.audio_generation.fetch_add(1, Ordering::AcqRel);
        self.change(|state| {
            state.video = VideoState::Failed(message.clone());
            state.audio = AudioState::Failed(message);
            state.frame = None;
        });
    }
    fn cancelled(&self, generation: u64) -> bool {
        self.closing.load(Ordering::Acquire)
            || !self.video.load(Ordering::Acquire)
            || self.generation.load(Ordering::Acquire) != generation
    }
}

#[derive(Clone)]
pub struct MediaHandle {
    shared: Arc<Shared>,
}
impl MediaHandle {
    pub fn serial(&self) -> &str {
        self.shared.config.serial.as_str()
    }
    pub fn start_video(&self) -> bool {
        let Ok(mut state) = self.shared.update.lock() else {
            return false;
        };
        if self.shared.closing.load(Ordering::Acquire)
            || self.shared.poisoned.load(Ordering::Acquire)
            || self.shared.video.load(Ordering::Acquire)
        {
            return false;
        }
        self.shared.generation.fetch_add(1, Ordering::AcqRel);
        state.video = VideoState::Starting;
        state.frame = None;
        state.revision = state.revision.wrapping_add(1);
        self.shared.video.store(true, Ordering::Release);
        true
    }
    pub fn stop_video(&self) {
        self.shared.change(|state| {
            self.shared.video.store(false, Ordering::Release);
            self.shared.generation.fetch_add(1, Ordering::AcqRel);
            if !self.shared.poisoned.load(Ordering::Acquire) {
                state.video = if self.shared.video_busy.load(Ordering::Acquire) {
                    VideoState::Stopping
                } else {
                    VideoState::Stopped
                };
                state.frame = None;
            }
        });
    }
    pub fn start_audio(&self) -> bool {
        let Ok(mut state) = self.shared.update.lock() else {
            return false;
        };
        if self.shared.closing.load(Ordering::Acquire)
            || self.shared.poisoned.load(Ordering::Acquire)
            || self.shared.audio.load(Ordering::Acquire)
        {
            return false;
        }
        self.shared.audio_generation.fetch_add(1, Ordering::AcqRel);
        state.audio = AudioState::Starting;
        state.audio_diagnostics.clear();
        state.revision = state.revision.wrapping_add(1);
        self.shared.audio.store(true, Ordering::Release);
        true
    }
    pub fn stop_audio(&self) {
        self.shared.change(|state| {
            self.shared.audio.store(false, Ordering::Release);
            self.shared.audio_generation.fetch_add(1, Ordering::AcqRel);
            if !self.shared.poisoned.load(Ordering::Acquire) {
                state.audio = if self.shared.audio_busy.load(Ordering::Acquire) {
                    AudioState::Stopping
                } else {
                    AudioState::Stopped
                };
            }
        });
    }
    /// Latest frame only: slow views never queue an unbounded history of images.
    /// This method does not wait for a contended state lock.
    pub fn take_update(&self, last_revision: &mut u64) -> Option<Update> {
        let mut state = self.shared.update.try_lock().ok()?;
        if state.revision == *last_revision {
            return None;
        }
        *last_revision = state.revision;
        Some(Update {
            revision: state.revision,
            video: state.video.clone(),
            audio: state.audio.clone(),
            audio_diagnostics: state.audio_diagnostics.clone(),
            frame: state.frame.take(),
        })
    }
    pub fn shutdown(&self) {
        self.shared.closing.store(true, Ordering::Release);
        self.stop_video();
        self.stop_audio();
    }
}

/// Owns two idle workers. Construction does not contact ADB or start audio.
/// After the native event loop exits, call `close` for bounded observable cleanup.
pub struct MediaSession {
    handle: MediaHandle,
    workers: Vec<JoinHandle<()>>,
}
impl MediaSession {
    pub fn new(config: MediaConfig) -> Result<Self, MediaError> {
        config.validate()?;
        let shared = Arc::new(Shared {
            config,
            video: AtomicBool::new(false),
            audio: AtomicBool::new(false),
            video_busy: AtomicBool::new(false),
            audio_busy: AtomicBool::new(false),
            closing: AtomicBool::new(false),
            generation: AtomicU64::new(0),
            audio_generation: AtomicU64::new(0),
            poisoned: AtomicBool::new(false),
            update: Mutex::new(Update {
                revision: 1,
                video: VideoState::Stopped,
                audio: AudioState::Stopped,
                audio_diagnostics: String::new(),
                frame: None,
            }),
        });
        let handle = MediaHandle { shared };
        let mut workers = Vec::new();
        for (name, worker) in [
            ("rex-frames", video_worker as fn(Arc<Shared>)),
            ("rex-audio", audio_worker),
        ] {
            let shared = handle.shared.clone();
            match thread::Builder::new()
                .name(name.into())
                .spawn(move || worker(shared))
            {
                Ok(thread) => workers.push(thread),
                Err(error) => {
                    handle.shutdown();
                    let deadline = Instant::now() + Duration::from_millis(500);
                    for thread in workers {
                        while !thread.is_finished() && Instant::now() < deadline {
                            thread::sleep(Duration::from_millis(5));
                        }
                        if thread.is_finished() {
                            let _ = thread.join();
                        }
                    }
                    return Err(MediaError::Process(format!(
                        "Could not start media worker: {error}"
                    )));
                }
            }
        }
        Ok(Self { handle, workers })
    }
    pub fn handle(&self) -> MediaHandle {
        self.handle.clone()
    }
    /// Never call this wait on the UI event thread. Kill/reap belongs to workers.
    pub fn close(&mut self, timeout: Duration) -> Result<(), MediaError> {
        self.handle.shutdown();
        let deadline = Instant::now() + timeout;
        while self.workers.iter().any(|t| !t.is_finished()) {
            if Instant::now() >= deadline {
                return Err(MediaError::Cleanup("Media workers did not finish within the shutdown deadline; cleanup is unverified".into()));
            }
            thread::sleep(Duration::from_millis(5));
        }
        let mut panicked = false;
        for worker in self.workers.drain(..) {
            // Drain/join every finished worker even if an earlier one panicked.
            // A retry must never erase a known cleanup failure.
            panicked |= worker.join().is_err();
        }
        if panicked {
            self.handle
                .shared
                .poison("A media worker panicked; cleanup is unverified".into());
            return Err(MediaError::Cleanup(
                "A media worker panicked; cleanup is unverified".into(),
            ));
        }
        if self.handle.shared.poisoned.load(Ordering::Acquire) {
            return Err(MediaError::Cleanup(
                "An owned media process could not be reaped within its deadline".into(),
            ));
        }
        Ok(())
    }
}
impl Drop for MediaSession {
    fn drop(&mut self) {
        self.handle.shutdown();
    }
}

fn video_worker(shared: Arc<Shared>) {
    let mut sequence = 0_u64;
    while !shared.closing.load(Ordering::Acquire) {
        if !shared.video.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(10));
            continue;
        }
        shared.video_busy.store(true, Ordering::Release);
        let generation = shared.generation.load(Ordering::Acquire);
        let started = Instant::now();
        if shared.config.video_backend == VideoBackend::Screenrecord {
            let result = capture_stream(&shared, generation, &mut sequence);
            if let Err(MediaError::Cleanup(message)) = &result {
                shared.poison(message.clone());
                break;
            }
            shared.video_idle();
            match result {
                _ if shared.cancelled(generation) => continue,
                result => {
                    let message = result.err().map(|e| e.to_string()).unwrap_or_else(|| {
                        "Screenrecord stream ended; start capture again explicitly".into()
                    });
                    shared.change(|state| {
                        if !shared.cancelled(generation) {
                            shared.video.store(false, Ordering::Release);
                            state.video = VideoState::Failed(message);
                            state.frame = None;
                        }
                    });
                    continue;
                }
            }
        }
        let result = capture(&shared, generation).and_then(|bytes| {
            if shared.cancelled(generation) {
                return Err(MediaError::Cancelled);
            }
            sequence = sequence.wrapping_add(1);
            decode_png(&bytes, sequence, started)
        });
        if let Err(MediaError::Cleanup(message)) = &result {
            shared.poison(message.clone());
            break;
        }
        shared.video_idle();
        if !shared.cancelled(generation) {
            match result {
                Ok(frame) => shared.change(|s| {
                    if !shared.cancelled(generation) {
                        s.video = VideoState::Capturing { frames: sequence };
                        s.frame = Some(frame);
                    }
                }),
                Err(MediaError::Cancelled) => {}
                Err(error) => {
                    shared.change(|s| {
                        if !shared.cancelled(generation) {
                            shared.video.store(false, Ordering::Release);
                            s.video = VideoState::Failed(error.to_string());
                            s.frame = None;
                        }
                    });
                }
            }
        }
        while !shared.cancelled(generation) && started.elapsed() < shared.config.capture_interval {
            thread::sleep(Duration::from_millis(5));
        }
    }
}

#[cfg(target_os = "linux")]
fn capture(shared: &Shared, generation: u64) -> Result<Vec<u8>, MediaError> {
    let mut command = process::adb_command(&shared.config);
    command.args([
        "-s",
        shared.config.serial.as_str(),
        "exec-out",
        "screencap",
        "-p",
    ]);
    let output = process::bounded_output(
        command,
        shared.config.capture_timeout,
        MAX_PNG_BYTES,
        || shared.cancelled(generation),
    )?;
    if !output.status.success() {
        return Err(MediaError::Process(format!(
            "ADB screenshot failed (exit {:?}): {}",
            output.status.code(),
            process::diagnostic(&output.stderr)
        )));
    }
    // A server-start notice on stderr can accompany a valid PNG; malformed image
    // data remains rejected by the decoder, independent of a zero process exit.
    Ok(output.stdout)
}
#[cfg(not(target_os = "linux"))]
fn capture(_shared: &Shared, _generation: u64) -> Result<Vec<u8>, MediaError> {
    Err(MediaError::Unsupported)
}

#[cfg(target_os = "linux")]
fn audio_worker(shared: Arc<Shared>) {
    process::audio_loop(shared);
}
#[cfg(not(target_os = "linux"))]
fn audio_worker(shared: Arc<Shared>) {
    while !shared.closing.load(Ordering::Acquire) {
        if shared.audio.swap(false, Ordering::AcqRel) {
            shared.change(|s| s.audio = AudioState::Failed(MediaError::Unsupported.to_string()));
        }
        thread::sleep(Duration::from_millis(10));
    }
}

#[cfg(target_os = "linux")]
fn capture_stream(shared: &Shared, generation: u64, sequence: &mut u64) -> Result<(), MediaError> {
    stream::capture_stream(
        &shared.config,
        || shared.cancelled(generation),
        |mut frame| {
            *sequence = sequence.wrapping_add(1);
            frame.number = *sequence;
            shared.change(|state| {
                if !shared.cancelled(generation) {
                    state.video = VideoState::Capturing { frames: *sequence };
                    state.frame = Some(frame);
                }
            });
        },
    )
}
#[cfg(not(target_os = "linux"))]
fn capture_stream(
    _shared: &Shared,
    _generation: u64,
    _sequence: &mut u64,
) -> Result<(), MediaError> {
    Err(MediaError::Unsupported)
}

#[cfg(test)]
mod state_tests {
    use super::*;
    #[test]
    fn panicked_worker_cleanup_failure_stays_latched_on_retry() {
        let config = MediaConfig::new(Serial::new("unit-test").unwrap());
        let mut session = MediaSession::new(config).unwrap();
        session
            .workers
            .push(thread::spawn(|| panic!("injected test worker failure")));
        assert!(matches!(
            session.close(Duration::from_secs(1)),
            Err(MediaError::Cleanup(_))
        ));
        assert!(session.workers.is_empty());
        assert!(matches!(
            session.close(Duration::from_secs(1)),
            Err(MediaError::Cleanup(_))
        ));
        assert!(!session.handle().start_video());
    }
    #[test]
    fn cleanup_failure_terminally_cancels_pending_restarts() {
        let mut config = MediaConfig::new(Serial::new("unit-test").unwrap());
        config.adb = "/not-installed/adb".into();
        config.scrcpy = "/not-installed/scrcpy".into();
        let mut session = MediaSession::new(config).unwrap();
        let handle = session.handle();
        // Inject the otherwise exceptional kernel-cleanup-failure transition.
        handle.shared.poison("test cleanup failure".into());
        assert!(handle.shared.cancelled(0));
        assert!(!handle.shared.video.load(Ordering::Acquire));
        assert!(!handle.shared.audio.load(Ordering::Acquire));
        assert!(!handle.start_video());
        assert!(!handle.start_audio());
        assert!(matches!(
            session.close(Duration::from_secs(1)),
            Err(MediaError::Cleanup(_))
        ));
    }
}
