//! Unprivileged entry point for a separately provisioned Waydroid runtime.
//! No initialization, service/session lifecycle, privileged changes, shell, or
//! rendering implementation lives here. Unknown backend state fails closed.

use std::ffi::{OsStr, OsString};
#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
use std::io::{self, Read};
use std::path::PathBuf;
#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
use std::process::Child;
use std::process::{Command, Stdio};
#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
use std::thread;
use std::time::Duration;
#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
use std::time::Instant;

pub const HELP: &str = "RexPlayer native launcher prototype (Linux x86_64/aarch64 Waydroid only)
Usage: rex-launcher <doctor|status|launch> [--waydroid PATH] [--timeout-ms N]
       rex-launcher launch --allow-session-start [options]

  doctor  Inspect local environment and existing Waydroid status
  status  Read existing Waydroid session/container status
  launch  Request full UI for an existing same-user/display RUNNING session

Options:
  --waydroid PATH  Existing trusted Waydroid executable (default: waydroid on PATH)
  --timeout-ms N   Diagnostic deadline, 100..30000 ms (default: 3000)
  --allow-session-start  Required for launch: upstream may start/unfreeze a session
  -h, --help      Show this help

Never installs, initializes, or directly starts/stops sessions/services.
Explicit launch uses upstream show-full-ui, which can start/unfreeze a session
if state changes after the check. It requires --allow-session-start acknowledgement.
This CLI does not render a GPUI shell or verify Android frames; see core/ui for the separate native shell.";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Doctor,
    Status,
    Launch,
}

#[derive(Clone, Debug)]
pub struct Options {
    pub action: Action,
    pub executable: OsString,
    pub timeout: Duration,
    pub allow_session_start: bool,
}

impl Options {
    pub fn parse(args: impl IntoIterator<Item = OsString>) -> Result<Option<Self>, String> {
        let mut args = args.into_iter();
        let Some(action) = args.next() else {
            return Err("A command is required".into());
        };
        if action == "--help" || action == "-h" {
            if args.next().is_some() {
                return Err("Help does not accept extra arguments".into());
            }
            return Ok(None);
        }
        let action = match action.to_str() {
            Some("doctor") => Action::Doctor,
            Some("status") => Action::Status,
            Some("launch") => Action::Launch,
            _ => return Err("Unsupported command; expected doctor, status, or launch".into()),
        };
        let mut executable = None;
        let mut timeout = None;
        let mut allow_session_start = false;
        while let Some(arg) = args.next() {
            match arg.to_str() {
                Some("--waydroid") if executable.is_none() => {
                    let value = args.next().ok_or("--waydroid requires a path")?;
                    if value.is_empty() || value.to_string_lossy().starts_with('-') {
                        return Err("--waydroid requires a nonempty executable path".into());
                    }
                    executable = Some(value);
                }
                Some("--timeout-ms") if timeout.is_none() => {
                    let value = args.next().ok_or("--timeout-ms requires a number")?;
                    let millis = value
                        .to_str()
                        .and_then(|s| s.parse::<u64>().ok())
                        .filter(|n| (100..=30_000).contains(n))
                        .ok_or("--timeout-ms must be an integer from 100 to 30000")?;
                    timeout = Some(Duration::from_millis(millis));
                }
                Some("--allow-session-start")
                    if !allow_session_start && action == Action::Launch =>
                {
                    allow_session_start = true;
                }
                _ => return Err("Unknown, repeated, or command-inapplicable option".into()),
            }
        }
        Ok(Some(Self {
            action,
            executable: executable.unwrap_or_else(|| "waydroid".into()),
            timeout: timeout.unwrap_or(Duration::from_secs(3)),
            allow_session_start,
        }))
    }
}

#[derive(Clone, Debug)]
pub struct HostContext {
    pub os: String,
    pub arch: String,
    pub effective_uid: Option<u32>,
    pub wayland_display: Option<String>,
    pub runtime_dir: Option<PathBuf>,
}

impl HostContext {
    pub fn current() -> Self {
        // Read the effective UID from the kernel, never trust USER/UID variables.
        let effective_uid = std::fs::read_to_string("/proc/self/status")
            .ok()
            .and_then(|s| {
                s.lines()
                    .find_map(|line| line.strip_prefix("Uid:"))
                    .and_then(|ids| ids.split_whitespace().nth(1))
                    .and_then(|id| id.parse().ok())
            });
        Self {
            os: std::env::consts::OS.into(),
            arch: std::env::consts::ARCH.into(),
            effective_uid,
            wayland_display: std::env::var("WAYLAND_DISPLAY")
                .ok()
                .filter(|s| !s.is_empty()),
            runtime_dir: std::env::var_os("XDG_RUNTIME_DIR")
                .filter(|s| !s.is_empty())
                .map(PathBuf::from),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Running,
    Stopped,
    Frozen,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RuntimeStatus {
    pub session: State,
    pub container: Option<State>,
    pub session_uid: Option<u32>,
    pub wayland_display: Option<String>,
}

fn set_once<T>(slot: &mut Option<T>, value: T) -> Result<(), String> {
    if slot.replace(value).is_some() {
        Err("Duplicate status field".into())
    } else {
        Ok(())
    }
}

impl RuntimeStatus {
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let text = std::str::from_utf8(bytes).map_err(|_| "Status is not UTF-8")?;
        if text
            .chars()
            .any(|c| c.is_control() && c != '\n' && c != '\r' && c != '\t')
        {
            return Err("Unexpected control characters in status".into());
        }
        let (mut session, mut container, mut session_uid, mut wayland_display) =
            (None, None, None, None);
        let (mut vendor, mut ip) = (None, None);
        for line in text.lines().filter(|line| !line.trim().is_empty()) {
            let (key, value) = line.split_once(':').ok_or("Unrecognized status output")?;
            let value = value.trim();
            match key.trim() {
                "Session" => {
                    let value = match value {
                        "RUNNING" => State::Running,
                        "STOPPED" => State::Stopped,
                        _ => return Err("Unknown session state".into()),
                    };
                    set_once(&mut session, value)?;
                }
                "Container" => {
                    let value = match value {
                        "RUNNING" => State::Running,
                        "STOPPED" => State::Stopped,
                        "FROZEN" => State::Frozen,
                        _ => return Err("Unknown container state".into()),
                    };
                    set_once(&mut container, value)?;
                }
                "Session user" => {
                    let (name, uid) = value.rsplit_once('(').ok_or("Invalid session user")?;
                    if name.is_empty() {
                        return Err("Missing session user name".into());
                    }
                    let uid = uid
                        .strip_suffix(')')
                        .and_then(|s| s.parse().ok())
                        .ok_or("Invalid session user ID")?;
                    set_once(&mut session_uid, uid)?;
                }
                "Wayland display" if !value.is_empty() => {
                    set_once(&mut wayland_display, value.to_owned())?;
                }
                "Vendor type" => set_once(&mut vendor, value)?,
                "IP address" => set_once(&mut ip, value)?,
                _ => return Err("Unrecognized status field".into()),
            }
        }
        let session = session.ok_or("Missing session state")?;
        if session == State::Running
            && (container.is_none() || session_uid.is_none() || wayland_display.is_none())
        {
            return Err("Running session status is incomplete".into());
        }
        if session == State::Stopped
            && (container.is_some() || session_uid.is_some() || wayland_display.is_some())
        {
            return Err("Stopped session status is inconsistent".into());
        }
        Ok(Self {
            session,
            container,
            session_uid,
            wayland_display,
        })
    }

    fn ready(&self, host: &HostContext) -> Result<(), &'static str> {
        if self.session != State::Running || self.container != Some(State::Running) {
            return Err("Waydroid session and container must already be RUNNING");
        }
        if self.session_uid != host.effective_uid {
            return Err("Waydroid belongs to another user; refusing to control it");
        }
        if host.wayland_display.is_none() || host.runtime_dir.is_none() {
            return Err("WAYLAND_DISPLAY and XDG_RUNTIME_DIR are required for UI readiness");
        }
        if self.wayland_display != host.wayland_display {
            return Err("Waydroid belongs to a different Wayland display");
        }
        if !host.runtime_dir.as_ref().is_some_and(|p| p.is_absolute()) {
            return Err("XDG_RUNTIME_DIR must be an absolute path");
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BackendError {
    MissingExecutable,
    UnsupportedPlatform,
    Timeout,
    OutputLimit,
    Io(String),
}

#[derive(Debug)]
pub struct CommandOutput {
    pub success: bool,
    pub code: Option<i32>,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}

/// Injection boundary used by hermetic tests. Diagnostics are bounded; UI
/// requests are explicit asynchronous operations with no rendering assertion.
pub trait Backend {
    fn execute(
        &mut self,
        executable: &OsStr,
        args: &[&str],
        timeout: Duration,
    ) -> Result<CommandOutput, BackendError>;

    /// Start only the approved UI command. The caller does not own Android lifecycle.
    fn spawn_ui(&mut self, executable: &OsStr) -> Result<u32, BackendError>;
}

pub struct ProcessBackend;

impl Backend for ProcessBackend {
    fn spawn_ui(&mut self, executable: &OsStr) -> Result<u32, BackendError> {
        if !cfg!(all(
            target_os = "linux",
            any(target_arch = "x86_64", target_arch = "aarch64")
        )) {
            return Err(BackendError::UnsupportedPlatform);
        }
        let child = Command::new(executable)
            .arg("show-full-ui")
            .stdin(Stdio::null())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .spawn()
            .map_err(|e| {
                if e.kind() == std::io::ErrorKind::NotFound {
                    BackendError::MissingExecutable
                } else {
                    BackendError::Io(e.to_string())
                }
            })?;
        // Deliberately transfer ownership to the user/OS. The short-lived CLI
        // exits immediately and never kills a UI or Waydroid session on drop.
        Ok(child.id())
    }

    fn execute(
        &mut self,
        executable: &OsStr,
        args: &[&str],
        timeout: Duration,
    ) -> Result<CommandOutput, BackendError> {
        #[cfg(all(
            target_os = "linux",
            any(target_arch = "x86_64", target_arch = "aarch64")
        ))]
        {
            bounded_command(executable, args, timeout)
        }
        #[cfg(not(all(
            target_os = "linux",
            any(target_arch = "x86_64", target_arch = "aarch64")
        )))]
        {
            let _ = (executable, args, timeout);
            Err(BackendError::UnsupportedPlatform)
        }
    }
}

#[derive(Debug)]
pub struct Report {
    pub exit_code: i32,
    pub message: String,
}

fn report(exit_code: i32, message: impl Into<String>) -> Report {
    Report {
        exit_code,
        message: message.into(),
    }
}

pub fn run(options: &Options, host: &HostContext, backend: &mut impl Backend) -> Report {
    if host.os != "linux" || !matches!(host.arch.as_str(), "x86_64" | "aarch64") {
        return report(
            69,
            "Unsupported platform: this launcher supports Linux x86_64/aarch64 Waydroid only",
        );
    }
    if host.effective_uid == Some(0) || host.effective_uid.is_none() {
        return report(
            69,
            "Run as an ordinary Linux user; a non-root effective UID is required",
        );
    }
    if options.action == Action::Launch && !options.allow_session_start {
        return report(78, "Launch requires --allow-session-start: upstream show-full-ui may start or unfreeze your Waydroid session even after a successful status check. No command was executed");
    }
    let output = match backend.execute(&options.executable, &["status"], options.timeout) {
        Ok(output) => output,
        Err(BackendError::MissingExecutable) => {
            return report(
                69,
                "Waydroid executable not found; provision it separately before using this launcher",
            );
        }
        Err(BackendError::Timeout) => {
            return report(
                75,
                "Waydroid diagnostic timed out; launch readiness is unknown",
            );
        }
        Err(error) => return report(70, format!("Waydroid diagnostic failed: {error:?}")),
    };
    if !output.success {
        return report(
            70,
            format!(
                "Waydroid status failed (exit {:?}); launch readiness is unknown",
                output.code
            ),
        );
    }
    // A success exit alone is not sufficient: upstream may print initialization
    // notices and return zero. Any stderr also makes this narrow contract fail closed.
    if !output.stderr.is_empty() {
        return report(
            70,
            "Waydroid status wrote diagnostics to stderr; readiness is unknown",
        );
    }
    let status = match RuntimeStatus::parse(&output.stdout) {
        Ok(status) => status,
        Err(error) => return report(70, format!("Cannot establish Waydroid state: {error}")),
    };
    let summary = format!(
        "Session: {:?}; container: {:?}",
        status.session, status.container
    );
    if options.action == Action::Status {
        return report(
            0,
            format!("{summary}\nRead-only status; UI rendering has not been verified"),
        );
    }
    if let Err(reason) = status.ready(host) {
        return report(
            3,
            format!("{summary}\nNot ready: {reason}. No session or service was changed"),
        );
    }
    if options.action == Action::Doctor {
        return report(0, format!("{summary}\nExisting same-user/display session is ready for further integration\nLaunch requires explicit --allow-session-start acknowledgement; rendered frames are unverified"));
    }
    // The explicit flag acknowledges that upstream may auto-start/unfreeze on a
    // race. We still require the strict, current, same-user/display status gate.
    match backend.spawn_ui(&options.executable) {
        Ok(pid) => report(0, format!("{summary}\nWaydroid UI request process spawned (PID {pid}). Command completion and rendered frames are unverified\nThe UI/session remains user-owned; this launcher will not stop it")),
        Err(error) => report(70, format!("UI request could not be spawned: {error:?}")),
    }
}

#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
fn nonblocking<T: std::os::fd::AsRawFd>(pipe: &T) -> io::Result<()> {
    // Linux fcntl flags. The fd remains owned by ChildStdout/ChildStderr.
    unsafe extern "C" {
        fn fcntl(fd: i32, cmd: i32, ...) -> i32;
    }
    const F_GETFL: i32 = 3;
    const F_SETFL: i32 = 4;
    const O_NONBLOCK: i32 = 0x800;
    let fd = pipe.as_raw_fd();
    // SAFETY: fd is an open pipe borrowed for this call, with valid Linux flags.
    let flags = unsafe { fcntl(fd, F_GETFL) };
    if flags < 0 || unsafe { fcntl(fd, F_SETFL, flags | O_NONBLOCK) } < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
fn drain(pipe: &mut impl Read, bytes: &mut Vec<u8>) -> Result<bool, BackendError> {
    const LIMIT: usize = 64 * 1024;
    let mut buffer = [0u8; 4096];
    // Bound work per tick even when a faulty diagnostic writes continuously.
    for _ in 0..16 {
        match pipe.read(&mut buffer) {
            Ok(0) => return Ok(true),
            Ok(n) => {
                if bytes.len() + n > LIMIT {
                    return Err(BackendError::OutputLimit);
                }
                bytes.extend_from_slice(&buffer[..n]);
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(false),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(BackendError::Io(error.to_string())),
        }
    }
    Ok(false)
}

#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
fn terminate_owned_child(child: &mut Child) {
    // Only this diagnostic child is ours. Never kill by name/process group or
    // stop Waydroid's externally owned session/container. Reap when possible.
    let _ = child.kill();
    let deadline = Instant::now() + Duration::from_millis(250);
    loop {
        match child.try_wait() {
            Ok(Some(_)) | Err(_) => break,
            Ok(None) if Instant::now() < deadline => thread::sleep(Duration::from_millis(5)),
            Ok(None) => break,
        }
    }
}

#[cfg(all(
    target_os = "linux",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
fn bounded_command(
    executable: &OsStr,
    args: &[&str],
    timeout: Duration,
) -> Result<CommandOutput, BackendError> {
    let mut child = Command::new(executable)
        .args(args)
        .env("LC_ALL", "C")
        .env("LANG", "C")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| {
            if error.kind() == std::io::ErrorKind::NotFound {
                BackendError::MissingExecutable
            } else {
                BackendError::Io(error.to_string())
            }
        })?;
    let result = (|| {
        let mut stdout = child
            .stdout
            .take()
            .ok_or_else(|| BackendError::Io("Missing stdout pipe".into()))?;
        let mut stderr = child
            .stderr
            .take()
            .ok_or_else(|| BackendError::Io("Missing stderr pipe".into()))?;
        nonblocking(&stdout)
            .and_then(|_| nonblocking(&stderr))
            .map_err(|e| BackendError::Io(e.to_string()))?;
        let deadline = Instant::now() + timeout;
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let (mut stdout_eof, mut stderr_eof) = (false, false);
        loop {
            if Instant::now() >= deadline {
                return Err(BackendError::Timeout);
            }
            if !stdout_eof {
                stdout_eof = drain(&mut stdout, &mut out)?;
            }
            if !stderr_eof {
                stderr_eof = drain(&mut stderr, &mut err)?;
            }
            let status = child
                .try_wait()
                .map_err(|e| BackendError::Io(e.to_string()))?;
            if let Some(status) = status {
                if stdout_eof && stderr_eof {
                    return Ok(CommandOutput {
                        success: status.success(),
                        code: status.code(),
                        stdout: out,
                        stderr: err,
                    });
                }
            }
            thread::sleep(Duration::from_millis(5));
        }
    })();
    if result.is_err() {
        terminate_owned_child(&mut child);
    }
    result
}
