#![cfg(all(target_os = "linux", feature = "linux-adapter"))]

use evdev::{AbsoluteAxisCode, InputEvent};
use rex_input_core::{AndroidSize, Rotation};
use rex_input_linux::{DeviceConfig, DeviceState, FrameTransport, TouchAdapter};
use rex_keymap::{
    linux::{LinuxInputSession, SessionError},
    HostEvent, HostKey, Keymap, KeymapController, WindowViewport,
};
use std::{cell::RefCell, io, rc::Rc};

#[derive(Default)]
struct Observed {
    frames: Vec<Vec<InputEvent>>,
    attempts: usize,
    fail_at: Option<usize>,
}
struct MemoryTransport(Rc<RefCell<Observed>>);
impl FrameTransport for MemoryTransport {
    fn emit_frame(&mut self, events: &[InputEvent]) -> io::Result<()> {
        let mut state = self.0.borrow_mut();
        state.attempts += 1;
        if state.fail_at == Some(state.attempts) {
            state.fail_at = None;
            return Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "injected write failure",
            ));
        }
        state.frames.push(events.to_vec());
        Ok(())
    }
}
fn setup() -> (LinuxInputSession<MemoryTransport>, Rc<RefCell<Observed>>) {
    let size = AndroidSize::new(1001, 1001).unwrap();
    let c = KeymapController::new(
        Keymap::default(),
        WindowViewport::new(0.0, 0.0, 1000.0, 1000.0).unwrap(),
        size,
        Rotation::None,
    )
    .unwrap();
    let state = Rc::new(RefCell::new(Observed::default()));
    let adapter = TouchAdapter::with_transport(
        DeviceConfig::new(size, c.max_contacts()).unwrap(),
        MemoryTransport(Rc::clone(&state)),
    );
    (LinuxInputSession::with_adapter(c, adapter).unwrap(), state)
}
fn down(key: &str) -> HostEvent {
    HostEvent::KeyDown {
        key: HostKey::new(key).unwrap(),
        repeat: false,
    }
}

#[test]
fn real_adapter_translation_splits_taps_and_joystick_starts_into_distinct_frames() {
    let (mut session, observed) = setup();
    session.handle(HostEvent::FocusGained).unwrap();
    assert_eq!(session.handle(down("e")).unwrap(), 2);
    assert_eq!(observed.borrow().frames.len(), 2);
    {
        let frames = &observed.borrow().frames;
        assert!(frames[0]
            .iter()
            .any(|e| e.code() == AbsoluteAxisCode::ABS_MT_TRACKING_ID.0 && e.value() >= 0));
        assert!(frames[1]
            .iter()
            .any(|e| e.code() == AbsoluteAxisCode::ABS_MT_TRACKING_ID.0 && e.value() == -1));
    }
    assert_eq!(session.handle(down("w")).unwrap(), 2);
    assert_eq!(session.handle(down("space")).unwrap(), 1);
    assert_eq!(session.submitted_active_count(), Some(2));
    assert_eq!(session.handle(HostEvent::FocusLost).unwrap(), 2);
    assert_eq!(session.submitted_active_count(), Some(0));
    session.close().unwrap();
}

#[test]
fn partial_tap_failure_gates_source_discards_batch_and_requires_explicit_resync() {
    let (mut session, observed) = setup();
    session.handle(HostEvent::FocusGained).unwrap();
    observed.borrow_mut().fail_at = Some(2);
    assert!(matches!(
        session.handle(down("e")),
        Err(SessionError::Adapter(_))
    ));
    assert_eq!(observed.borrow().frames.len(), 1);
    assert_eq!(session.adapter_state(), DeviceState::Faulted);
    assert!(session.is_blocked());
    assert_eq!(session.controller().active_count(), 0);
    assert_eq!(session.submitted_active_count(), None);
    assert!(matches!(
        session.handle(HostEvent::FocusGained),
        Err(SessionError::Blocked)
    ));
    assert_eq!(observed.borrow().attempts, 2);
    session.resynchronize().unwrap();
    assert!(!session.controller().focused());
    assert_eq!(session.handle(down("space")).unwrap(), 0);
    session.handle(HostEvent::FocusGained).unwrap();
    assert_eq!(session.handle(down("space")).unwrap(), 1);
    assert_eq!(session.submitted_active_count(), Some(1));
    session.close().unwrap();
    let state = observed.borrow();
    assert!(state
        .frames
        .last()
        .unwrap()
        .iter()
        .any(|e| e.code() == AbsoluteAxisCode::ABS_MT_TRACKING_ID.0 && e.value() == -1));
}

#[test]
fn failed_cleanup_stays_blocked_and_guest_dimension_change_is_rejected() {
    let (mut session, observed) = setup();
    session.handle(HostEvent::FocusGained).unwrap();
    session.handle(down("space")).unwrap();
    assert!(matches!(
        session.set_geometry(
            WindowViewport::new(0.0, 0.0, 10.0, 10.0).unwrap(),
            AndroidSize::new(20, 20).unwrap(),
            Rotation::None
        ),
        Err(SessionError::DestinationChanged)
    ));
    assert_eq!(session.submitted_active_count(), Some(1));
    observed.borrow_mut().fail_at = Some(2);
    assert!(session.resynchronize().is_err());
    assert!(session.is_blocked());
    assert_eq!(session.submitted_active_count(), None);
    session.close().unwrap();
}

#[test]
fn mismatched_adapter_is_rejected_without_writes() {
    let size = AndroidSize::new(100, 100).unwrap();
    let c = KeymapController::new(
        Keymap::default(),
        WindowViewport::new(0.0, 0.0, 100.0, 100.0).unwrap(),
        size,
        Rotation::None,
    )
    .unwrap();
    let state = Rc::new(RefCell::new(Observed::default()));
    let adapter = TouchAdapter::with_transport(
        DeviceConfig::new(size, 1).unwrap(),
        MemoryTransport(Rc::clone(&state)),
    );
    assert!(matches!(
        LinuxInputSession::with_adapter(c, adapter),
        Err(SessionError::IncompatibleAdapter)
    ));
    assert_eq!(state.borrow().attempts, 0);
}
