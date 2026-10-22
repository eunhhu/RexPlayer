//! Optional H.264 compatibility streaming through a trusted local FFmpeg decoder.
//! No shared-texture/zero-copy or audiovisual synchronization claim.
use crate::{Frame, MAX_DIMENSION, MAX_PIXELS, MediaConfig, MediaError};
use std::io::{self, Read};
use std::os::fd::AsRawFd;
use std::os::unix::process::CommandExt;
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const TICK: Duration = Duration::from_millis(5);
const DIAGNOSTIC_LIMIT: usize = 65536;

struct OwnedProcess(Option<Child>);
impl OwnedProcess {
    fn spawn(command: &mut Command) -> Result<Self, MediaError> {
        command.process_group(0);
        command
            .spawn()
            .map(|child| Self(Some(child)))
            .map_err(|e| MediaError::Process(format!("Could not start media stream process: {e}")))
    }
    fn child(&mut self) -> &mut Child {
        self.0.as_mut().expect("owned unreaped child")
    }
    fn exited(&self) -> Result<Option<bool>, MediaError> {
        let child = self.0.as_ref().expect("owned unreaped child");
        // SAFETY: Child owns this unreaped positive PID. WNOWAIT keeps its identity
        // reserved until group cleanup; no PID-derived signal follows reaping.
        let mut info: libc::siginfo_t = unsafe { std::mem::zeroed() };
        let rc = unsafe {
            libc::waitid(
                libc::P_PID,
                child.id(),
                &mut info,
                libc::WEXITED | libc::WNOHANG | libc::WNOWAIT,
            )
        };
        if rc != 0 {
            return Err(MediaError::Process(format!(
                "Could not inspect stream child: {}",
                io::Error::last_os_error()
            )));
        }
        // SAFETY: waitid initialized this siginfo_t for a child-status event.
        if unsafe { info.si_pid() } == 0 {
            return Ok(None);
        }
        Ok(Some(
            info.si_code == libc::CLD_EXITED && unsafe { info.si_status() } == 0,
        ))
    }
    fn close(&mut self) -> Result<(), MediaError> {
        let Some(mut child) = self.0.take() else {
            return Ok(());
        };
        // SAFETY: process_group(0) made this still-unreaped child the group leader.
        // Kill only this owned group, including decoder helpers, then reap leader.
        let rc = unsafe { libc::kill(-(child.id() as libc::pid_t), libc::SIGKILL) };
        let kill_error =
            if rc == -1 && io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH) {
                Some(io::Error::last_os_error().to_string())
            } else {
                None
            };
        let deadline = Instant::now() + Duration::from_millis(500);
        loop {
            match child.try_wait() {
                Ok(Some(_)) => {
                    return kill_error.map_or(Ok(()), |e| {
                        Err(MediaError::Cleanup(format!(
                            "Could not terminate stream group: {e}"
                        )))
                    });
                }
                Ok(None) if Instant::now() < deadline => thread::sleep(TICK),
                result => {
                    let reason = match result {
                        Err(e) => e.to_string(),
                        _ => "stream child reap deadline exceeded".into(),
                    };
                    let _ = thread::Builder::new()
                        .name("rex-stream-reaper".into())
                        .spawn(move || {
                            let _ = child.wait();
                        });
                    return Err(MediaError::Cleanup(reason));
                }
            }
        }
    }
}
impl Drop for OwnedProcess {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

fn nonblocking(pipe: &impl AsRawFd) -> Result<(), MediaError> {
    // SAFETY: pipe is borrowed and open for the duration of these fd operations.
    let flags = unsafe { libc::fcntl(pipe.as_raw_fd(), libc::F_GETFL) };
    if flags == -1
        || unsafe { libc::fcntl(pipe.as_raw_fd(), libc::F_SETFL, flags | libc::O_NONBLOCK) } == -1
    {
        return Err(MediaError::Process(io::Error::last_os_error().to_string()));
    }
    Ok(())
}

#[derive(Default)]
struct PpmFrames {
    buffer: Vec<u8>,
    frame_count: u64,
}
impl PpmFrames {
    fn push(
        &mut self,
        bytes: &[u8],
        started: Instant,
        emit: &mut impl FnMut(Frame),
    ) -> Result<(), MediaError> {
        if self.buffer.len().saturating_add(bytes.len()) > MAX_PIXELS as usize * 3 + 65536 {
            return Err(MediaError::OutputLimit);
        }
        self.buffer.extend_from_slice(bytes);
        loop {
            let Some(end) = self
                .buffer
                .iter()
                .enumerate()
                .filter(|(_, b)| **b == b'\n')
                .nth(2)
                .map(|(i, _)| i + 1)
            else {
                if self.buffer.len() > 128 {
                    return Err(MediaError::Decode("Invalid PPM frame header".into()));
                }
                return Ok(());
            };
            if end > 128 {
                return Err(MediaError::Decode("PPM header exceeds limit".into()));
            }
            let header = std::str::from_utf8(&self.buffer[..end])
                .map_err(|_| MediaError::Decode("Non-ASCII PPM header".into()))?;
            let fields: Vec<_> = header.split_ascii_whitespace().collect();
            if fields.len() != 4 || fields[0] != "P6" || fields[3] != "255" {
                return Err(MediaError::Decode(
                    "Unsupported FFmpeg PPM frame format".into(),
                ));
            }
            let width: u32 = fields[1]
                .parse()
                .map_err(|_| MediaError::Decode("Invalid frame width".into()))?;
            let height: u32 = fields[2]
                .parse()
                .map_err(|_| MediaError::Decode("Invalid frame height".into()))?;
            let pixels = u64::from(width) * u64::from(height);
            if width == 0
                || height == 0
                || width > MAX_DIMENSION
                || height > MAX_DIMENSION
                || pixels > MAX_PIXELS
            {
                return Err(MediaError::Decode("Stream dimensions exceed limit".into()));
            }
            let size = pixels as usize * 3;
            if self.buffer.len() < end + size {
                return Ok(());
            }
            let mut bgra = Vec::with_capacity(pixels as usize * 4);
            for rgb in self.buffer[end..end + size].as_chunks::<3>().0 {
                bgra.extend_from_slice(&[rgb[2], rgb[1], rgb[0], 255]);
            }
            self.buffer.drain(..end + size);
            self.frame_count += 1;
            emit(Frame {
                number: self.frame_count,
                width,
                height,
                bgra,
                captured_at: Instant::now(),
                capture_duration: started.elapsed(),
            });
        }
    }
}

fn diagnostic_read(pipe: &mut impl Read, diagnostic: &mut Vec<u8>) -> Result<(), MediaError> {
    let mut bytes = [0u8; 4096];
    for _ in 0..4 {
        match pipe.read(&mut bytes) {
            Ok(0) => break,
            Ok(n) if diagnostic.len() + n <= DIAGNOSTIC_LIMIT => {
                diagnostic.extend_from_slice(&bytes[..n])
            }
            Ok(_) => return Err(MediaError::OutputLimit),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(MediaError::Process(e.to_string())),
        }
    }
    Ok(())
}

pub(crate) fn capture_stream(
    config: &MediaConfig,
    cancelled: impl Fn() -> bool,
    on_frame: impl FnMut(Frame),
) -> Result<(), MediaError> {
    let mut source = crate::process::adb_command(config);
    source.args([
        "-s",
        config.serial.as_str(),
        "exec-out",
        "screenrecord",
        "--output-format=h264",
        "-",
    ]);
    pipeline(
        source,
        &config.ffmpeg,
        config.stream_stall_timeout,
        cancelled,
        on_frame,
    )
}

fn pipeline(
    mut source: Command,
    ffmpeg: &std::ffi::OsStr,
    timeout: Duration,
    cancelled: impl Fn() -> bool,
    mut on_frame: impl FnMut(Frame),
) -> Result<(), MediaError> {
    if cancelled() {
        return Err(MediaError::Cancelled);
    }
    source
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut producer = OwnedProcess::spawn(&mut source)?;
    let mut decoder_command = Command::new(ffmpeg);
    decoder_command
        .args([
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-max_alloc",
            "67108864",
            "-max_pixels",
            "16777216",
            "-threads",
            "2",
            "-probesize",
            "32",
            "-analyzeduration",
            "0",
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-an",
            "-f",
            "image2pipe",
            "-vcodec",
            "ppm",
            "-threads",
            "2",
            "pipe:1",
        ])
        .stdin(Stdio::from(
            producer
                .child()
                .stdout
                .take()
                .expect("configured source stdout"),
        ))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut decoder = match OwnedProcess::spawn(&mut decoder_command) {
        Ok(decoder) => decoder,
        Err(error) => {
            producer.close()?;
            return Err(error);
        }
    };
    let mut pixels = decoder
        .child()
        .stdout
        .take()
        .expect("configured decoder stdout");
    let mut decoder_err = decoder
        .child()
        .stderr
        .take()
        .expect("configured decoder stderr");
    let mut producer_err = producer
        .child()
        .stderr
        .take()
        .expect("configured producer stderr");
    let result = (|| {
        nonblocking(&pixels)?;
        nonblocking(&decoder_err)?;
        nonblocking(&producer_err)?;
        let mut parser = PpmFrames::default();
        let mut diagnostics = Vec::new();
        let mut last_frame = Instant::now();
        let mut buffer = [0u8; 16384];
        loop {
            if cancelled() {
                return Err(MediaError::Cancelled);
            }
            if last_frame.elapsed() > timeout {
                return Err(MediaError::Timeout);
            }
            diagnostic_read(&mut decoder_err, &mut diagnostics)?;
            diagnostic_read(&mut producer_err, &mut diagnostics)?;
            let mut eof = false;
            // Bound each iteration so a flooded pipe cannot starve cancellation.
            for _ in 0..16 {
                match pixels.read(&mut buffer) {
                    Ok(0) => {
                        eof = true;
                        break;
                    }
                    Ok(n) => {
                        parser.push(&buffer[..n], last_frame, &mut |frame| {
                            last_frame = Instant::now();
                            on_frame(frame);
                        })?;
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                    Err(e) => return Err(MediaError::Process(e.to_string())),
                }
            }
            if producer.exited()? == Some(false) || decoder.exited()? == Some(false) {
                return Err(MediaError::Process(format!(
                    "Android video stream/decoder failed: {}",
                    crate::process::diagnostic(&diagnostics)
                )));
            }
            if eof && decoder.exited()?.is_some() && producer.exited()?.is_some() {
                if !parser.buffer.is_empty() || parser.frame_count == 0 {
                    return Err(MediaError::Decode(
                        "Video stream ended without complete frames".into(),
                    ));
                }
                return Ok(());
            }
            thread::sleep(TICK);
        }
    })();
    // Try both cleanups even when one fails. Cleanup failure overrides normal EOF
    // or cancellation so the caller can latch the session against repeated starts.
    let decoder_cleanup = decoder.close();
    let producer_cleanup = producer.close();
    decoder_cleanup?;
    producer_cleanup?;
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fragmented_frames_decode_without_channel_swap_or_backlog() {
        let bytes = b"P6\n2 1\n255\n\x01\x02\x03\x04\x05\x06P6\n1 1\n255\n\x10\x20\x30";
        let mut parser = PpmFrames::default();
        let mut frames = Vec::new();
        for chunk in bytes.chunks(2) {
            parser
                .push(chunk, Instant::now(), &mut |f| frames.push(f))
                .unwrap();
        }
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].bgra, [3, 2, 1, 255, 6, 5, 4, 255]);
        assert_eq!(frames[1].bgra, [0x30, 0x20, 0x10, 255]);
        assert!(parser.buffer.is_empty());
    }
    #[test]
    fn malformed_and_oversized_headers_are_rejected() {
        for header in [
            "P5\n1 1\n255\n",
            "P6\n0 1\n255\n",
            "P6\n9000 1\n255\n",
            "P6\n8192 8192\n255\n",
            "P6\n1 1\n65535\n",
        ] {
            assert!(
                PpmFrames::default()
                    .push(header.as_bytes(), Instant::now(), &mut |_| {})
                    .is_err()
            );
        }
        assert!(
            PpmFrames::default()
                .push(&[b'a'; 129], Instant::now(), &mut |_| {})
                .is_err()
        );
    }
    #[test]
    fn missing_decoder_closes_owned_source_promptly() {
        let mut source = Command::new("sleep");
        source.arg("30");
        let start = Instant::now();
        let error = pipeline(
            source,
            std::ffi::OsStr::new("rex-nonexistent-ffmpeg-fixture"),
            Duration::from_secs(2),
            || false,
            |_| {},
        )
        .unwrap_err();
        assert!(matches!(error, MediaError::Process(_)));
        assert!(start.elapsed() < Duration::from_secs(2));
    }
    #[test]
    fn cancellation_before_spawn_does_not_run_source() {
        let error = pipeline(
            Command::new("rex-nonexistent-source-fixture"),
            std::ffi::OsStr::new("ffmpeg"),
            Duration::from_secs(2),
            || true,
            |_| {},
        )
        .unwrap_err();
        assert_eq!(error, MediaError::Cancelled);
    }
    #[test]
    #[ignore = "requires installed trusted ffmpeg; explicitly run in codec validation"]
    fn real_ffmpeg_stalled_pipeline_cancels_and_times_out_promptly() {
        for cancel in [false, true] {
            let mut source = Command::new("sleep");
            source.arg("30");
            let start = Instant::now();
            let error = pipeline(
                source,
                std::ffi::OsStr::new("ffmpeg"),
                Duration::from_millis(150),
                || cancel && start.elapsed() > Duration::from_millis(50),
                |_| {},
            )
            .unwrap_err();
            assert_eq!(
                error,
                if cancel {
                    MediaError::Cancelled
                } else {
                    MediaError::Timeout
                }
            );
            assert!(start.elapsed() < Duration::from_secs(2));
        }
    }
    #[test]
    #[ignore = "requires installed trusted ffmpeg and generated H264 fixture; explicitly run in codec validation"]
    fn real_ffmpeg_decodes_three_h264_frames() {
        let fixture = std::env::var_os("REX_H264_FIXTURE")
            .expect("set REX_H264_FIXTURE to generated 64x48/3-frame fixture");
        let mut source = Command::new("cat");
        source.arg(fixture);
        let mut frames = Vec::new();
        pipeline(
            source,
            std::ffi::OsStr::new("ffmpeg"),
            Duration::from_secs(10),
            || false,
            |frame| frames.push(frame),
        )
        .unwrap();
        assert_eq!(frames.len(), 3);
        assert!(
            frames
                .iter()
                .all(|f| f.width == 64 && f.height == 48 && f.bgra.len() == 64 * 48 * 4)
        );
    }
}
