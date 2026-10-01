//! A bounded, nonblocking frontend boundary for explicit Linux input enablement.
//!
//! All device construction, writes, and cleanup occur on one worker thread. GUI
//! methods never wait on device I/O. Lifecycle interruption or queue overflow closes
//! the session permanently; a fresh, explicit enable is required. Shutdown signals
//! are out-of-band, so they cannot be dropped behind a full event queue. In-flight
//! OS operations cannot be interrupted here; [`InputWorker::wait_closed`] reports
//! whether cleanup actually completed within the caller's shutdown deadline.

use crate::{
    linux::{LinuxInputSession, SessionError},
    HostEvent, Keymap, KeymapController, WindowViewport,
};
use rex_input_core::{AndroidSize, Rotation};
use rex_input_linux::FrameTransport;
use std::{
    fmt, io,
    sync::{
        atomic::{AtomicBool, AtomicU8, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TrySendError},
        Arc, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

/// Maximum queued host events; no unbounded event backlog is retained.
pub const INPUT_QUEUE_CAPACITY: usize = 64;
const CHECK_INTERVAL: Duration = Duration::from_millis(10);
const STOP_USER: u8 = 1;
const STOP_FOCUS: u8 = 2;
const STOP_EMERGENCY: u8 = 3;
const STOP_OVERFLOW: u8 = 4;

/// Observable worker lifecycle. Ready is transport readiness, not guest delivery.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WorkerState {
    /// Device construction has been requested on the worker.
    Starting,
    /// Device is available to this process; guest visibility is still unverified.
    Ready,
    /// Source is gated and cleanup is pending on the worker.
    Stopping,
    /// Explicit cleanup succeeded and the device was destroyed.
    Closed,
    /// Startup, translation, write, or cleanup failed; input cannot resume.
    Failed,
}

/// Snapshot for a GUI status indicator. Never interpreted as Android acknowledgment.
#[derive(Debug, Clone)]
pub struct WorkerStatus {
    /// Current lifecycle phase.
    pub state: WorkerState,
    /// Known submitted contacts, or unknown after an I/O error/during startup.
    pub active_contacts: Option<usize>,
    /// Actionable error or lifecycle explanation, including missing route verification.
    pub message: Option<String>,
}

/// A source event could not be accepted. The caller must not retry the same event.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SendError {
    /// A lifecycle interruption has permanently gated this worker.
    Disabled,
    /// Queue filled; all queued input will be discarded and the device closed.
    Full,
    /// Worker has exited; a fresh explicit enable is required.
    Disconnected,
}
impl fmt::Display for SendError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Disabled => {
                "input worker is stopping or disabled; explicitly enable a new session"
            }
            Self::Full => "input queue filled; input disabled and pending events discarded",
            Self::Disconnected => "input worker exited; inspect its status before enabling again",
        })
    }
}
impl std::error::Error for SendError {}

enum Command {
    Event(HostEvent),
    Viewport(WindowViewport),
    Wake,
}
struct Shared {
    stop: AtomicU8,
    terminated: AtomicBool,
    status: Mutex<WorkerStatus>,
}

/// Window-owned handle. Send/resize/shutdown are nonblocking and never perform I/O.
/// Dropping requests asynchronous cleanup; retain the handle to observe completion.
#[derive(Debug)]
pub struct InputWorker {
    sender: SyncSender<Command>,
    shared: Arc<Shared>,
    thread: JoinHandle<()>,
}
impl fmt::Debug for Shared {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Shared")
            .field("stop", &self.stop.load(Ordering::Acquire))
            .field("terminated", &self.terminated.load(Ordering::Acquire))
            .finish_non_exhaustive()
    }
}

impl InputWorker {
    /// Explicitly starts one device-owning thread. Construction returns immediately;
    /// inspect status for permission/device errors. Only this requested thread opens
    /// `/dev/uinput`; no permissions, guest routing, or global input capture are set up.
    pub fn spawn(
        keymap: Keymap,
        viewport: WindowViewport,
        size: AndroidSize,
        rotation: Rotation,
    ) -> io::Result<Self> {
        Self::spawn_factory(move || {
            let controller = KeymapController::new(keymap, viewport, size, rotation)
                .map_err(SessionError::Input)?;
            LinuxInputSession::create(controller)
        })
    }

    /// Queues a window event without waiting. Focus loss, emergency release, and
    /// fresh Escape down instead gate and close out-of-band. Repeat Escape is ignored.
    /// After overflow, never retry: discard the host gesture and await terminal status.
    pub fn send(&self, event: HostEvent) -> Result<(), SendError> {
        match &event {
            HostEvent::FocusLost => return self.request_stop(STOP_FOCUS),
            HostEvent::EmergencyRelease => return self.request_stop(STOP_EMERGENCY),
            HostEvent::KeyDown { key, repeat: false } if key.as_str() == "escape" => {
                return self.request_stop(STOP_EMERGENCY);
            }
            _ => {}
        }
        self.enqueue(Command::Event(event))
    }

    /// Queues a viewport resize, canceling old contacts before later input. If the
    /// bounded queue is full, input closes fail-closed. Guest-size changes require
    /// shutting down and explicitly constructing another worker with the new size.
    pub fn set_viewport(&self, viewport: WindowViewport) -> Result<(), SendError> {
        self.enqueue(Command::Viewport(viewport))
    }

    /// Requests out-of-band shutdown. Does not wait or claim releases were delivered.
    /// Idempotent; cleanup completion is observable through status/wait_closed.
    pub fn shutdown(&self) {
        let _ = self.request_stop(STOP_USER);
    }

    /// Returns the latest status without waiting on a lock or device operation.
    /// `None` means the worker is momentarily publishing another status; poll later.
    pub fn status(&self) -> Option<WorkerStatus> {
        if self.thread.is_finished() && !self.shared.terminated.load(Ordering::Acquire) {
            return Some(WorkerStatus {
                state: WorkerState::Failed,
                active_contacts: None,
                message: Some(
                    "input worker terminated unexpectedly; release delivery is unknown".to_owned(),
                ),
            });
        }
        let mut result = self.shared.status.try_lock().ok()?.clone();
        if self.shared.stop.load(Ordering::Acquire) != 0
            && matches!(result.state, WorkerState::Starting | WorkerState::Ready)
        {
            result.state = WorkerState::Stopping;
            result.message = Some("input disabled; waiting for worker device cleanup".to_owned());
        }
        Some(result)
    }

    /// Waits at most the supplied duration for the worker to exit. Use only outside
    /// the UI event loop, e.g. after the application window closes. `true` means the
    /// thread ended; inspect status to distinguish successful cleanup from failure.
    pub fn wait_closed(&self, timeout: Duration) -> bool {
        let started = Instant::now();
        while !self.thread.is_finished() {
            let remaining = timeout.saturating_sub(started.elapsed());
            if remaining.is_zero() {
                return false;
            }
            thread::sleep(remaining.min(CHECK_INTERVAL));
        }
        true
    }

    fn enqueue(&self, command: Command) -> Result<(), SendError> {
        if self.shared.stop.load(Ordering::Acquire) != 0 {
            return Err(SendError::Disabled);
        }
        match self.sender.try_send(command) {
            Ok(()) => Ok(()),
            Err(TrySendError::Full(_)) => {
                let _ = self.request_stop(STOP_OVERFLOW);
                Err(SendError::Full)
            }
            Err(TrySendError::Disconnected(_)) => Err(SendError::Disconnected),
        }
    }

    fn request_stop(&self, reason: u8) -> Result<(), SendError> {
        if self.thread.is_finished() {
            return Err(SendError::Disconnected);
        }
        let _ = self
            .shared
            .stop
            .compare_exchange(0, reason, Ordering::AcqRel, Ordering::Acquire);
        // The flag is authoritative even if the wake cannot fit in a full queue.
        let _ = self.sender.try_send(Command::Wake);
        Ok(())
    }

    fn spawn_factory<T, F>(factory: F) -> io::Result<Self>
    where
        T: FrameTransport + Send + 'static,
        F: FnOnce() -> Result<LinuxInputSession<T>, SessionError> + Send + 'static,
    {
        let (sender, receiver) = mpsc::sync_channel(INPUT_QUEUE_CAPACITY);
        let shared = Arc::new(Shared {
            stop: AtomicU8::new(0),
            terminated: AtomicBool::new(false),
            status: Mutex::new(WorkerStatus {
                state: WorkerState::Starting,
                active_contacts: None,
                message: Some("opening the explicitly requested virtual touchscreen".to_owned()),
            }),
        });
        let state = Arc::clone(&shared);
        let thread = thread::Builder::new()
            .name("rex-window-input".to_owned())
            .spawn(move || {
                run(factory, receiver, &state);
                state.terminated.store(true, Ordering::Release);
            })?;
        Ok(Self {
            sender,
            shared,
            thread,
        })
    }
}
impl Drop for InputWorker {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn publish(shared: &Shared, state: WorkerState, active_contacts: Option<usize>, message: String) {
    if let Ok(mut status) = shared.status.lock() {
        *status = WorkerStatus {
            state,
            active_contacts,
            message: Some(message),
        };
    }
}

fn run<T, F>(factory: F, receiver: Receiver<Command>, shared: &Shared)
where
    T: FrameTransport,
    F: FnOnce() -> Result<LinuxInputSession<T>, SessionError>,
{
    if shared.stop.load(Ordering::Acquire) != 0 {
        publish(
            shared,
            WorkerState::Closed,
            Some(0),
            "input canceled before device creation".to_owned(),
        );
        return;
    }
    let mut session = match factory() {
        Ok(session) => session,
        Err(error) => {
            publish(shared, WorkerState::Failed, None, error.to_string());
            return;
        }
    };
    publish(
        shared,
        WorkerState::Ready,
        Some(0),
        "virtual touchscreen ready; Android device routing and receipt are unverified".to_owned(),
    );
    let mut failure = None;
    loop {
        if shared.stop.load(Ordering::Acquire) != 0 {
            break;
        }
        let command = match receiver.recv_timeout(CHECK_INTERVAL) {
            Ok(command) => command,
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => break,
        };
        // A lifecycle flag arriving while recv was waiting wins over queued input.
        if shared.stop.load(Ordering::Acquire) != 0 {
            break;
        }
        let result = match command {
            Command::Event(event) => session.handle(event),
            Command::Viewport(viewport) => session.set_viewport(viewport),
            Command::Wake => continue,
        };
        match result {
            Ok(_) => publish(
                shared,
                WorkerState::Ready,
                session.submitted_active_count(),
                "virtual touchscreen ready; Android device routing and receipt are unverified"
                    .to_owned(),
            ),
            Err(error) => {
                failure = Some(error.to_string());
                break;
            }
        }
    }
    // Drop all queued gestures. Cleanup uses the adapter's own submitted state,
    // including full resynchronization after a possibly partial transport write.
    drop(receiver);
    let cleanup = session.close();
    let reason = match shared.stop.load(Ordering::Acquire) {
        STOP_FOCUS => "window lost focus; input closed; explicitly enable again",
        STOP_EMERGENCY => "emergency release; input closed; explicitly enable again",
        STOP_OVERFLOW => "input queue overflow; pending events discarded; explicitly enable again",
        _ => "input closed",
    };
    match (failure, cleanup) {
        (None, Ok(())) => publish(shared, WorkerState::Closed, Some(0), reason.to_owned()),
        (Some(error), Ok(())) => publish(
            shared,
            WorkerState::Failed,
            Some(0),
            format!("{error}; device cleanup succeeded; explicitly enable again"),
        ),
        (previous, Err(error)) => {
            let previous = previous.map(|e| format!("{e}; ")).unwrap_or_default();
            publish(
                shared,
                WorkerState::Failed,
                None,
                format!("{previous}device cleanup failed: {error}; release delivery is unknown"),
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use evdev::InputEvent;
    use rex_input_linux::{DeviceConfig, TouchAdapter};

    #[derive(Default)]
    struct Observed {
        frames: usize,
        fail: bool,
        dropped: bool,
    }
    struct MemoryTransport(Arc<Mutex<Observed>>);
    impl FrameTransport for MemoryTransport {
        fn emit_frame(&mut self, _events: &[InputEvent]) -> io::Result<()> {
            let mut state = self.0.lock().unwrap();
            if state.fail {
                return Err(io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "injected worker failure",
                ));
            }
            state.frames += 1;
            Ok(())
        }
    }
    impl Drop for MemoryTransport {
        fn drop(&mut self) {
            self.0.lock().unwrap().dropped = true;
        }
    }
    fn session(state: Arc<Mutex<Observed>>) -> LinuxInputSession<MemoryTransport> {
        let size = AndroidSize::new(100, 100).unwrap();
        let controller = KeymapController::new(
            Keymap::default(),
            WindowViewport::new(0.0, 0.0, 100.0, 100.0).unwrap(),
            size,
            Rotation::None,
        )
        .unwrap();
        let adapter = TouchAdapter::with_transport(
            DeviceConfig::new(size, controller.max_contacts()).unwrap(),
            MemoryTransport(state),
        );
        LinuxInputSession::with_adapter(controller, adapter).unwrap()
    }
    fn down() -> HostEvent {
        HostEvent::KeyDown {
            key: crate::HostKey::new("space").unwrap(),
            repeat: false,
        }
    }
    fn wait_for(worker: &InputWorker, predicate: impl Fn(&WorkerStatus) -> bool) -> WorkerStatus {
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if let Some(status) = worker.status() {
                if predicate(&status) {
                    return status;
                }
            }
            assert!(
                Instant::now() < deadline,
                "worker did not reach expected state"
            );
            thread::sleep(Duration::from_millis(1));
        }
    }

    #[test]
    fn focus_loss_closes_on_worker_and_manual_enable_is_required() {
        let observed = Arc::new(Mutex::new(Observed::default()));
        let state = Arc::clone(&observed);
        let worker = InputWorker::spawn_factory(move || Ok(session(state))).unwrap();
        worker.send(HostEvent::FocusGained).unwrap();
        worker.send(down()).unwrap();
        wait_for(&worker, |status| status.active_contacts == Some(1));
        worker.send(HostEvent::FocusLost).unwrap();
        assert!(worker.wait_closed(Duration::from_secs(2)));
        let status = worker.status().unwrap();
        assert_eq!(status.state, WorkerState::Closed);
        assert_eq!(status.active_contacts, Some(0));
        assert!(matches!(
            worker.send(down()),
            Err(SendError::Disabled | SendError::Disconnected)
        ));
        let state = observed.lock().unwrap();
        assert_eq!(state.frames, 2);
        assert!(state.dropped);
    }

    #[test]
    fn queue_overflow_is_nonblocking_discards_backlog_and_never_drops_shutdown() {
        let observed = Arc::new(Mutex::new(Observed::default()));
        let state = Arc::clone(&observed);
        let (entered_tx, entered_rx) = mpsc::channel();
        let (continue_tx, continue_rx) = mpsc::channel();
        let worker = InputWorker::spawn_factory(move || {
            entered_tx.send(()).unwrap();
            continue_rx.recv_timeout(Duration::from_secs(2)).unwrap();
            Ok(session(state))
        })
        .unwrap();
        entered_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        for _ in 0..INPUT_QUEUE_CAPACITY {
            worker.send(down()).unwrap();
        }
        assert_eq!(worker.send(down()), Err(SendError::Full));
        assert_eq!(worker.send(down()), Err(SendError::Disabled));
        assert_eq!(worker.status().unwrap().state, WorkerState::Stopping);
        assert!(!worker.wait_closed(Duration::from_millis(1)));
        continue_tx.send(()).unwrap();
        assert!(worker.wait_closed(Duration::from_secs(2)));
        assert_eq!(worker.status().unwrap().state, WorkerState::Closed);
        let state = observed.lock().unwrap();
        assert_eq!(state.frames, 0);
        assert!(state.dropped);
    }

    #[test]
    fn out_of_band_emergency_discards_queued_downs_during_startup() {
        let observed = Arc::new(Mutex::new(Observed::default()));
        let state = Arc::clone(&observed);
        let (entered_tx, entered_rx) = mpsc::channel();
        let (continue_tx, continue_rx) = mpsc::channel();
        let worker = InputWorker::spawn_factory(move || {
            entered_tx.send(()).unwrap();
            continue_rx.recv_timeout(Duration::from_secs(2)).unwrap();
            Ok(session(state))
        })
        .unwrap();
        entered_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        worker.send(HostEvent::FocusGained).unwrap();
        worker.send(down()).unwrap();
        worker.send(HostEvent::EmergencyRelease).unwrap();
        continue_tx.send(()).unwrap();
        assert!(worker.wait_closed(Duration::from_secs(2)));
        assert_eq!(observed.lock().unwrap().frames, 0);
        assert!(worker
            .status()
            .unwrap()
            .message
            .unwrap()
            .contains("emergency"));
    }

    #[test]
    fn write_and_cleanup_failure_are_observable_and_terminal() {
        let observed = Arc::new(Mutex::new(Observed {
            fail: true,
            ..Observed::default()
        }));
        let state = Arc::clone(&observed);
        let worker = InputWorker::spawn_factory(move || Ok(session(state))).unwrap();
        worker.send(HostEvent::FocusGained).unwrap();
        worker.send(down()).unwrap();
        assert!(worker.wait_closed(Duration::from_secs(2)));
        let status = worker.status().unwrap();
        assert_eq!(status.state, WorkerState::Failed);
        assert_eq!(status.active_contacts, None);
        assert!(status.message.unwrap().contains("cleanup failed"));
        assert!(observed.lock().unwrap().dropped);
    }

    #[test]
    fn startup_failure_is_reported_without_device_access() {
        let worker = InputWorker::spawn_factory::<MemoryTransport, _>(|| {
            Err(SessionError::Open(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "test denied",
            )))
        })
        .unwrap();
        assert!(worker.wait_closed(Duration::from_secs(2)));
        let status = worker.status().unwrap();
        assert_eq!(status.state, WorkerState::Failed);
        assert!(status.message.unwrap().contains("test denied"));
    }
}
