//! A bounded command mailbox and background process owner. No GPUI thread does
//! process I/O, sleeps, or waits for Waydroid. All status parsing stays upstream.
use crate::{Event, Request};
use rex_launcher::{Backend, BackendError, CommandOutput, HostContext, Options, ProcessBackend};
use std::ffi::{OsStr, OsString};
use std::io;
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TryRecvError};
use std::thread;
use std::time::Duration;

pub struct Worker {
    requests: SyncSender<Request>,
    events: Receiver<Event>,
}

impl Worker {
    pub fn start(executable: OsString, timeout: Duration) -> io::Result<Self> {
        let (requests, incoming) = mpsc::sync_channel::<Request>(1);
        let (outgoing, events) = mpsc::channel();
        thread::Builder::new()
            .name("rex-runtime".into())
            .spawn(move || {
                let mut backend = SupervisedBackend::default();
                let mut disconnected = false;
                loop {
                    if let Some(event) = backend.poll() {
                        let _ = outgoing.send(event);
                    }
                    if disconnected {
                        if backend.child.is_none() {
                            break;
                        }
                        // A closed view never signals/kills Android. Retain the
                        // request Child until reaped while our process is alive.
                        thread::sleep(Duration::from_millis(100));
                        continue;
                    }
                    match incoming.recv_timeout(Duration::from_millis(100)) {
                        Ok(request) => {
                            let options = Options {
                                action: request.action,
                                executable: executable.clone(),
                                timeout,
                                allow_session_start: request.allow_session_start,
                            };
                            let report =
                                rex_launcher::run(&options, &HostContext::current(), &mut backend);
                            if outgoing.send(Event::Finished { request, report }).is_err() {
                                disconnected = true;
                            }
                        }
                        Err(RecvTimeoutError::Disconnected) => disconnected = true,
                        Err(RecvTimeoutError::Timeout) => {}
                    }
                }
            })?;
        Ok(Self { requests, events })
    }

    /// Nonblocking, even if a faulty caller bypasses the controller busy guard.
    pub fn submit(&self, request: Request) -> Result<(), String> {
        self.requests
            .try_send(request)
            .map_err(|error| format!("Runtime worker could not accept the request: {error}"))
    }

    pub fn try_event(&self) -> Result<Event, TryRecvError> {
        self.events.try_recv()
    }
}

#[derive(Default)]
struct SupervisedBackend {
    child: Option<Child>,
    wait_error_reported: bool,
}

impl SupervisedBackend {
    fn poll(&mut self) -> Option<Event> {
        let child = self.child.as_mut()?;
        let pid = child.id();
        match child.try_wait() {
            Ok(Some(status)) => {
                self.child = None;
                self.wait_error_reported = false;
                Some(Event::UiExited {
                    pid,
                    code: status.code(),
                })
            }
            Ok(None) => None,
            Err(error) if !self.wait_error_reported => {
                self.wait_error_reported = true;
                Some(Event::UiWaitFailed {
                    pid,
                    message: error.to_string(),
                })
            }
            Err(_) => None,
        }
    }
}

impl Backend for SupervisedBackend {
    fn execute(
        &mut self,
        executable: &OsStr,
        args: &[&str],
        timeout: Duration,
    ) -> Result<CommandOutput, BackendError> {
        ProcessBackend.execute(executable, args, timeout)
    }

    fn spawn_ui(&mut self, executable: &OsStr) -> Result<u32, BackendError> {
        if self.child.is_some() {
            return Err(BackendError::Io(
                "A UI request process is already running".into(),
            ));
        }
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
            .map_err(|error| {
                if error.kind() == io::ErrorKind::NotFound {
                    BackendError::MissingExecutable
                } else {
                    BackendError::Io(error.to_string())
                }
            })?;
        let pid = child.id();
        self.child = Some(child);
        Ok(pid)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rex_launcher::Action;
    #[cfg(target_os = "linux")]
    use std::time::Instant;

    #[test]
    fn missing_runtime_reports_without_blocking_submit() {
        let worker = Worker::start(
            "/definitely-not-installed/rex-test-waydroid".into(),
            Duration::from_millis(100),
        )
        .unwrap();
        let request = Request {
            id: 1,
            action: Action::Doctor,
            allow_session_start: false,
        };
        worker.submit(request).unwrap();
        match worker.events.recv_timeout(Duration::from_secs(2)).unwrap() {
            Event::Finished {
                request: returned,
                report,
            } => {
                assert_eq!(returned, request);
                assert_eq!(report.exit_code, 69);
            }
            event => panic!("unexpected event: {event:?}"),
        }
    }

    #[test]
    fn unacknowledged_launch_executes_no_runtime() {
        let worker = Worker::start(
            "/definitely-not-installed/rex-test-waydroid".into(),
            Duration::from_millis(100),
        )
        .unwrap();
        worker
            .submit(Request {
                id: 1,
                action: Action::Launch,
                allow_session_start: false,
            })
            .unwrap();
        match worker.events.recv_timeout(Duration::from_secs(2)).unwrap() {
            Event::Finished { report, .. } => {
                // Root/unsupported host fails first, otherwise acknowledgement fails.
                let host = HostContext::current();
                if host.effective_uid == Some(0)
                    || host.effective_uid.is_none()
                    || host.os != "linux"
                    || !matches!(host.arch.as_str(), "x86_64" | "aarch64")
                {
                    assert_eq!(report.exit_code, 69);
                } else {
                    assert_eq!(report.exit_code, 78);
                    assert!(report.message.contains("No command was executed"));
                }
            }
            event => panic!("unexpected event: {event:?}"),
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn supervised_child_is_reaped_and_reports_its_real_exit() {
        // A harmless local test process exercises ownership without Waydroid,
        // Android, a shell command, or system/session state changes.
        let child = Command::new("/usr/bin/true").spawn().unwrap();
        let pid = child.id();
        let mut backend = SupervisedBackend {
            child: Some(child),
            wait_error_reported: false,
        };
        assert!(backend.spawn_ui(OsStr::new("/usr/bin/true")).is_err());
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if let Some(event) = backend.poll() {
                assert!(
                    matches!(event, Event::UiExited { pid: returned, code: Some(0) } if returned == pid)
                );
                break;
            }
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(5));
        }
        assert!(backend.child.is_none());
        assert!(backend.poll().is_none());
        assert!(!std::path::Path::new(&format!("/proc/{pid}")).exists());
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn ui_spawn_retains_handle_and_uses_nonzero_exit_without_claiming_rendering() {
        // GNU false ignores the show-full-ui argument and exits unsuccessfully.
        let mut backend = SupervisedBackend::default();
        let pid = backend.spawn_ui(OsStr::new("/usr/bin/false")).unwrap();
        assert_eq!(backend.child.as_ref().unwrap().id(), pid);
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if let Some(event) = backend.poll() {
                assert!(matches!(event, Event::UiExited { code: Some(1), .. }));
                break;
            }
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(5));
        }
    }
}
