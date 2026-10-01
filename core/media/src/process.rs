use crate::{AudioState, MediaConfig, MediaError, Shared};
use std::io::{self, Read};
use std::os::fd::AsRawFd;
use std::process::{Child, ChildStderr, Command, ExitStatus, Stdio};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread;
use std::time::{Duration, Instant};

const STDERR_LIMIT: usize = 64 * 1024;
const TICK: Duration = Duration::from_millis(5);

pub(crate) fn adb_command(config: &MediaConfig) -> Command {
    let mut command = Command::new(&config.adb);
    environment(&mut command);
    command
}

fn environment(command: &mut Command) {
    command
        .env("LC_ALL", "C")
        .env("LANG", "C")
        .env("ADB_MDNS_AUTO_CONNECT", "0")
        .env_remove("ANDROID_SERIAL")
        .env_remove("ADB_SERVER_SOCKET")
        .env_remove("ANDROID_ADB_SERVER_ADDRESS")
        .env_remove("ANDROID_ADB_SERVER_PORT")
        .env_remove("ADB_SERVER_HOST")
        .env_remove("ADB_SERVER_PORT");
    // This affects a newly started local ADB server, not an already running
    // server owned by the user. No connect, pair, tcpip, kill-server or discovery
    // command is issued. Ambient remote-server routing variables are removed.
}

pub(crate) struct Output {
    pub status: ExitStatus,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}

fn nonblocking(pipe: &impl AsRawFd) -> Result<(), MediaError> {
    let fd = pipe.as_raw_fd();
    // SAFETY: fd is a borrowed, live process pipe; constants use the host libc ABI.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags == -1 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } == -1 {
        return Err(MediaError::Process(format!(
            "Cannot make process pipe nonblocking: {}",
            io::Error::last_os_error()
        )));
    }
    Ok(())
}

fn drain(pipe: &mut impl Read, bytes: &mut Vec<u8>, limit: usize) -> Result<bool, MediaError> {
    let mut buffer = [0_u8; 16 * 1024];
    // Per-tick work is bounded even if a child floods a pipe continuously.
    for _ in 0..16 {
        match pipe.read(&mut buffer) {
            Ok(0) => return Ok(true),
            Ok(count) => {
                if count > limit.saturating_sub(bytes.len()) {
                    return Err(MediaError::OutputLimit);
                }
                bytes.extend_from_slice(&buffer[..count]);
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(false),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => {
                return Err(MediaError::Process(format!(
                    "Process pipe read failed: {error}"
                )));
            }
        }
    }
    Ok(false)
}

/// Kills only this exact owned host child, then waits up to 500 ms for reaping.
/// An exceptional kernel stall is handed to one reaper and poisons the session,
/// preventing repeated-start accumulation. No Android/global process is signaled.
fn terminate(mut child: Child, graceful: bool) -> Result<(), MediaError> {
    if child
        .try_wait()
        .map_err(|e| MediaError::Cleanup(e.to_string()))?
        .is_some()
    {
        return Ok(());
    }
    if graceful {
        // SAFETY: this positive PID belongs to an unreaped Child we own. SIGINT
        // asks scrcpy to run its normal socket/server/audio cleanup first.
        unsafe {
            libc::kill(child.id() as libc::pid_t, libc::SIGINT);
        }
        let deadline = Instant::now() + Duration::from_millis(250);
        while Instant::now() < deadline {
            if child
                .try_wait()
                .map_err(|e| MediaError::Cleanup(e.to_string()))?
                .is_some()
            {
                return Ok(());
            }
            thread::sleep(TICK);
        }
    }
    let _ = child.kill();
    let deadline = Instant::now() + Duration::from_millis(500);
    while Instant::now() < deadline {
        if child
            .try_wait()
            .map_err(|e| MediaError::Cleanup(e.to_string()))?
            .is_some()
        {
            return Ok(());
        }
        thread::sleep(TICK);
    }
    let _ = thread::Builder::new()
        .name("rex-media-reaper".into())
        .spawn(move || {
            let _ = child.wait();
        });
    Err(MediaError::Cleanup(
        "Owned media child did not reap within 500 ms; further media starts are blocked".into(),
    ))
}

pub(crate) fn bounded_output(
    mut command: Command,
    timeout: Duration,
    stdout_limit: usize,
    cancelled: impl Fn() -> bool,
) -> Result<Output, MediaError> {
    if cancelled() {
        return Err(MediaError::Cancelled);
    }
    let mut child = command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| MediaError::Process(format!("Could not start trusted ADB executable: {e}")))?;
    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| MediaError::Process("Missing screenshot output pipe".into()))?;
    let mut stderr = child
        .stderr
        .take()
        .ok_or_else(|| MediaError::Process("Missing screenshot diagnostic pipe".into()))?;
    let mut out = Vec::new();
    let mut err = Vec::new();
    let result = (|| {
        nonblocking(&stdout)?;
        nonblocking(&stderr)?;
        let deadline = Instant::now() + timeout;
        let mut exit = None;
        loop {
            if cancelled() {
                return Err(MediaError::Cancelled);
            }
            if Instant::now() >= deadline {
                return Err(MediaError::Timeout);
            }
            let out_done = drain(&mut stdout, &mut out, stdout_limit)?;
            let err_done = drain(&mut stderr, &mut err, STDERR_LIMIT)?;
            if exit.is_none() {
                exit = child
                    .try_wait()
                    .map_err(|e| MediaError::Process(e.to_string()))?;
            }
            if out_done
                && err_done
                && let Some(status) = exit
            {
                return Ok(status);
            }
            thread::sleep(TICK);
        }
    })();
    match result {
        Ok(status) => Ok(Output {
            status,
            stdout: out,
            stderr: err,
        }),
        Err(error) => {
            terminate(child, false)?;
            Err(error)
        }
    }
}

pub(crate) fn diagnostic(bytes: &[u8]) -> String {
    // Do not display terminal escapes/control codes from an external process.
    String::from_utf8_lossy(bytes)
        .chars()
        .filter(|c| !c.is_control() || matches!(c, '\n' | '\t'))
        .take(4096)
        .collect::<String>()
        .trim()
        .to_owned()
}

fn spawn_audio(config: &MediaConfig) -> Result<(Child, ChildStderr), MediaError> {
    let mut command = Command::new(&config.scrcpy);
    environment(&mut command);
    command
        .env("ADB", &config.adb)
        .arg(format!("--serial={}", config.serial.as_str()))
        .args([
            "--no-video",
            "--no-control",
            "--no-window",
            "--audio-source=output",
            "--require-audio",
            "--no-clipboard-autosync",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped());
    let mut child = command.spawn().map_err(|e| {
        MediaError::Process(format!(
            "Could not start trusted scrcpy audio executable: {e}"
        ))
    })?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| MediaError::Process("Missing audio diagnostic pipe".into()))?;
    if let Err(error) = nonblocking(&stderr) {
        terminate(child, false)?;
        return Err(error);
    }
    Ok((child, stderr))
}

pub(crate) fn audio_loop(shared: Arc<Shared>) {
    while !shared.closing.load(Ordering::Acquire) {
        if !shared.audio.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(10));
            continue;
        }
        shared.audio_busy.store(true, Ordering::Release);
        let generation = shared.audio_generation.load(Ordering::Acquire);
        let result = audio_request(&shared, generation);
        if let Err(MediaError::Cleanup(message)) = &result {
            shared.poison(message.clone());
            return;
        }
        shared.audio_idle();
        shared.change(|state| {
            if shared.audio_generation.load(Ordering::Acquire) == generation {
                shared.audio.store(false, Ordering::Release);
                state.audio = match result {
                    Ok(()) => AudioState::Stopped,
                    Err(error) => AudioState::Failed(error.to_string()),
                };
            }
        });
    }
}

fn audio_request(shared: &Shared, generation: u64) -> Result<(), MediaError> {
    if shared.closing.load(Ordering::Acquire)
        || shared.poisoned.load(Ordering::Acquire)
        || !shared.audio.load(Ordering::Acquire)
        || shared.audio_generation.load(Ordering::Acquire) != generation
    {
        return Ok(());
    }
    let (mut child, mut stderr) = spawn_audio(&shared.config)?;
    let pid = child.id();
    shared.change(|state| {
        if shared.audio_generation.load(Ordering::Acquire) == generation {
            state.audio = AudioState::ProcessRunning { pid };
        }
    });
    let mut diagnostics = Vec::new();
    loop {
        if shared.closing.load(Ordering::Acquire)
            || !shared.audio.load(Ordering::Acquire)
            || shared.audio_generation.load(Ordering::Acquire) != generation
        {
            return terminate(child, true);
        }
        let before = diagnostics.len();
        // Audio logs are bounded in total per request. Fail rather than allow an
        // external process to consume unbounded memory or busy-loop the worker.
        if let Err(error) = drain(&mut stderr, &mut diagnostics, STDERR_LIMIT) {
            terminate(child, true)?;
            return Err(error);
        }
        if diagnostics.len() != before {
            shared.change(|state| {
                if shared.audio_generation.load(Ordering::Acquire) == generation {
                    state.audio_diagnostics = diagnostic(&diagnostics);
                }
            });
        }
        match child.try_wait() {
            Ok(Some(status)) => {
                let details = diagnostic(&diagnostics);
                return Err(MediaError::Process(format!(
                    "scrcpy audio process exited ({:?}); playback is no longer running. {details}",
                    status.code()
                )));
            }
            Ok(None) => {}
            Err(error) => {
                terminate(child, true)?;
                return Err(MediaError::Process(format!(
                    "Could not read audio process status: {error}"
                )));
            }
        }
        thread::sleep(TICK);
    }
}
