#![cfg(target_os = "linux")]

use evdev::{AbsoluteAxisCode as Axis, EventType, InputEvent, KeyCode, SynchronizationCode};
use rex_input_core::{
    AndroidSize, BoundsPolicy, Contact, ContactAllocator, ContactId,
    ContactTransition as Transition, CoordinateMapping, PixelPoint, ReleaseReason, Rotation,
    Viewport, MAX_SLOTS, MAX_TRACKING_ID,
};
use rex_input_linux::{
    AdapterError, DeviceConfig, DeviceState, FrameTransport, InvalidTransition, TouchAdapter,
};
use std::{cell::RefCell, io, rc::Rc};

type Event = (u16, u16, i32);

#[derive(Debug)]
struct Observed {
    frames: Vec<Vec<Event>>,
    attempted: Vec<Vec<Event>>,
    accepted_prefix: Vec<Event>,
    fail_before: Option<usize>,
    selected_slot: usize,
    tracking: Vec<i32>,
    touch: bool,
    dropped: bool,
}

#[derive(Debug)]
struct MockTransport(Rc<RefCell<Observed>>);

impl FrameTransport for MockTransport {
    fn emit_frame(&mut self, events: &[InputEvent]) -> io::Result<()> {
        let mut state = self.0.borrow_mut();
        assert!(events
            .iter()
            .all(|e| e.event_type() != EventType::SYNCHRONIZATION));
        let mut frame: Vec<Event> = events
            .iter()
            .map(|e| (e.event_type().0, e.code(), e.value()))
            .collect();
        frame.push(sync());
        state.attempted.push(frame.clone());
        let fail_before = state.fail_before.take();
        for (index, event) in frame.iter().copied().enumerate() {
            if fail_before == Some(index) {
                return Err(io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "injected partial write",
                ));
            }
            state.accepted_prefix.push(event);
            match event {
                (kind, code, value) if kind == EventType::KEY.0 && code == KeyCode::BTN_TOUCH.0 => {
                    state.touch = value != 0;
                }
                (kind, code, value)
                    if kind == EventType::ABSOLUTE.0 && code == Axis::ABS_MT_SLOT.0 =>
                {
                    state.selected_slot = value as usize;
                }
                (kind, code, value)
                    if kind == EventType::ABSOLUTE.0 && code == Axis::ABS_MT_TRACKING_ID.0 =>
                {
                    let slot = state.selected_slot;
                    state.tracking[slot] = value;
                }
                _ => {}
            }
        }
        state.frames.push(frame);
        Ok(())
    }
}

impl Drop for MockTransport {
    fn drop(&mut self) {
        self.0.borrow_mut().dropped = true;
    }
}

fn setup(slots: usize) -> (TouchAdapter<MockTransport>, Rc<RefCell<Observed>>) {
    let state = Rc::new(RefCell::new(Observed {
        frames: Vec::new(),
        attempted: Vec::new(),
        accepted_prefix: Vec::new(),
        fail_before: None,
        selected_slot: 0,
        tracking: vec![-1; slots],
        touch: false,
        dropped: false,
    }));
    let config = DeviceConfig::new(AndroidSize::new(1080, 1920).unwrap(), slots).unwrap();
    (
        TouchAdapter::with_transport(config, MockTransport(state.clone())),
        state,
    )
}

fn contact(slot: usize, tracking: u32) -> Contact {
    Contact {
        id: ContactId(u64::from(tracking) + 100),
        slot,
        tracking_id: tracking,
        position: PixelPoint { x: 100, y: 200 },
    }
}

fn up(contact: Contact) -> Transition {
    Transition::Up {
        contact,
        reason: ReleaseReason::Up,
    }
}

fn touch(active: bool) -> Event {
    (EventType::KEY.0, KeyCode::BTN_TOUCH.0, i32::from(active))
}

fn abs(axis: Axis, value: i32) -> Event {
    (EventType::ABSOLUTE.0, axis.0, value)
}

fn sync() -> Event {
    (
        EventType::SYNCHRONIZATION.0,
        SynchronizationCode::SYN_REPORT.0,
        0,
    )
}

fn invalid(
    adapter: &mut TouchAdapter<MockTransport>,
    observed: &Rc<RefCell<Observed>>,
    transitions: &[Transition],
    expected: InvalidTransition,
) {
    let count = adapter.active_count();
    let attempts = observed.borrow().attempted.len();
    assert!(
        matches!(adapter.submit(transitions), Err(AdapterError::Invalid(error)) if error == expected)
    );
    assert_eq!(adapter.active_count(), count);
    assert_eq!(adapter.state(), DeviceState::Ready);
    assert_eq!(observed.borrow().attempted.len(), attempts);
}

#[test]
fn down_move_up_have_exact_type_b_events_and_one_synchronization_each() {
    let (mut adapter, observed) = setup(2);
    let mut c = contact(0, 0);
    adapter.submit(&[Transition::Down(c)]).unwrap();
    c.position = PixelPoint { x: 1079, y: 1919 };
    adapter.submit(&[Transition::Move(c)]).unwrap();
    adapter.submit(&[up(c)]).unwrap();
    assert_eq!(
        observed.borrow().frames,
        vec![
            vec![
                touch(true),
                abs(Axis::ABS_MT_SLOT, 0),
                abs(Axis::ABS_MT_TRACKING_ID, 0),
                abs(Axis::ABS_MT_POSITION_X, 100),
                abs(Axis::ABS_MT_POSITION_Y, 200),
                abs(Axis::ABS_X, 100),
                abs(Axis::ABS_Y, 200),
                sync()
            ],
            vec![
                touch(true),
                abs(Axis::ABS_MT_SLOT, 0),
                abs(Axis::ABS_MT_POSITION_X, 1079),
                abs(Axis::ABS_MT_POSITION_Y, 1919),
                abs(Axis::ABS_X, 1079),
                abs(Axis::ABS_Y, 1919),
                sync()
            ],
            vec![
                touch(false),
                abs(Axis::ABS_MT_SLOT, 0),
                abs(Axis::ABS_MT_TRACKING_ID, -1),
                sync()
            ],
        ]
    );
    assert_eq!(adapter.active_count(), Some(0));
}

#[test]
fn multitouch_batch_and_primary_handoff_keep_touch_active_until_last_release() {
    let (mut adapter, observed) = setup(3);
    let c0 = contact(0, 0);
    let mut c1 = contact(1, 1);
    c1.position = PixelPoint { x: 300, y: 400 };
    adapter
        .submit(&[Transition::Down(c0), Transition::Down(c1)])
        .unwrap();
    assert_eq!(
        observed.borrow().frames[0],
        vec![
            touch(true),
            abs(Axis::ABS_MT_SLOT, 0),
            abs(Axis::ABS_MT_TRACKING_ID, 0),
            abs(Axis::ABS_MT_POSITION_X, 100),
            abs(Axis::ABS_MT_POSITION_Y, 200),
            abs(Axis::ABS_MT_SLOT, 1),
            abs(Axis::ABS_MT_TRACKING_ID, 1),
            abs(Axis::ABS_MT_POSITION_X, 300),
            abs(Axis::ABS_MT_POSITION_Y, 400),
            abs(Axis::ABS_X, 100),
            abs(Axis::ABS_Y, 200),
            sync(),
        ]
    );
    adapter.submit(&[up(c0)]).unwrap();
    assert_eq!(
        observed.borrow().frames[1],
        vec![
            touch(true),
            abs(Axis::ABS_MT_SLOT, 0),
            abs(Axis::ABS_MT_TRACKING_ID, -1),
            abs(Axis::ABS_X, 300),
            abs(Axis::ABS_Y, 400),
            sync(),
        ]
    );
    let replacement = contact(0, 2);
    adapter.submit(&[Transition::Down(replacement)]).unwrap();
    let third = observed.borrow().frames[2].clone();
    assert_eq!(
        &third[third.len() - 3..],
        &[abs(Axis::ABS_X, 300), abs(Axis::ABS_Y, 400), sync()]
    );
    adapter.submit(&[up(c1), up(replacement)]).unwrap();
    assert_eq!(
        observed.borrow().frames[3],
        vec![
            touch(false),
            abs(Axis::ABS_MT_SLOT, 1),
            abs(Axis::ABS_MT_TRACKING_ID, -1),
            abs(Axis::ABS_MT_SLOT, 0),
            abs(Axis::ABS_MT_TRACKING_ID, -1),
            sync(),
        ]
    );
}

#[test]
fn core_focus_loss_produces_a_single_complete_release_frame_and_can_restart() {
    let (mut adapter, observed) = setup(10);
    let size = adapter.config().size();
    let mapping = CoordinateMapping::new(
        Viewport::new(10.0, 20.0, 1080.0, 1920.0).unwrap(),
        size,
        Rotation::None,
        BoundsPolicy::Reject,
    );
    let mut input = ContactAllocator::new(10, mapping).unwrap();
    let downs: Vec<_> = (0..10)
        .map(|id| input.down(ContactId(id), 10.0, 20.0).unwrap())
        .collect();
    adapter.submit(&downs).unwrap();
    assert_eq!(adapter.active_count(), Some(10));
    let releases = input.focus_lost();
    adapter.submit(&releases).unwrap();
    assert_eq!(adapter.active_count(), Some(0));
    assert_eq!(observed.borrow().tracking, vec![-1; 10]);
    assert!(!observed.borrow().touch);
    assert_eq!(observed.borrow().frames[1].len(), 22);
    assert_eq!(observed.borrow().frames[1][0], touch(false));
    assert_eq!(observed.borrow().frames[1].last(), Some(&sync()));
    adapter.submit(&input.focus_lost()).unwrap();
    assert_eq!(observed.borrow().frames.len(), 2);
    adapter
        .submit(&[input.down(ContactId(42), 1090.0, 1940.0).unwrap()])
        .unwrap();
    assert_eq!(adapter.active_count(), Some(1));
}

#[test]
fn malformed_transitions_are_rejected_without_writes_or_partial_local_mutation() {
    let (mut adapter, observed) = setup(2);
    let c = contact(0, 0);
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(contact(2, 0))],
        InvalidTransition::SlotOutOfRange,
    );
    let mut bad = c;
    bad.position.x = 1080;
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(bad)],
        InvalidTransition::PositionOutOfRange,
    );
    bad = c;
    bad.position.y = 1920;
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(bad)],
        InvalidTransition::PositionOutOfRange,
    );
    bad = c;
    bad.tracking_id = u32::MAX;
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(bad)],
        InvalidTransition::TrackingIdOutOfRange,
    );
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Move(c)],
        InvalidTransition::UnknownContact,
    );
    invalid(
        &mut adapter,
        &observed,
        &[up(c)],
        InvalidTransition::UnknownContact,
    );
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(c), up(c)],
        InvalidTransition::RepeatedSlot,
    );
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(c); 3],
        InvalidTransition::FrameTooLarge,
    );
    // The first transition is valid, the second fails; neither is committed or emitted.
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(c), Transition::Down(contact(2, 1))],
        InvalidTransition::SlotOutOfRange,
    );
    adapter.submit(&[Transition::Down(c)]).unwrap();
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(contact(0, 1))],
        InvalidTransition::SlotOccupied,
    );
    bad = contact(1, 1);
    bad.id = c.id;
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(bad)],
        InvalidTransition::DuplicateIdentity,
    );
    bad = c;
    bad.id = ContactId(987);
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Move(bad)],
        InvalidTransition::UnknownContact,
    );
    bad = c;
    bad.tracking_id = 1;
    invalid(
        &mut adapter,
        &observed,
        &[up(bad)],
        InvalidTransition::UnknownContact,
    );
    bad = c;
    bad.position.x += 1;
    invalid(
        &mut adapter,
        &observed,
        &[up(bad)],
        InvalidTransition::StaleRelease,
    );
    adapter.submit(&[up(c)]).unwrap();
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(c)],
        InvalidTransition::TrackingIdNotIncreasing,
    );
    adapter.submit(&[Transition::Down(contact(0, 1))]).unwrap();
}

#[test]
fn duplicate_identity_and_out_of_order_tracking_are_rejected_inside_a_batch() {
    let (mut adapter, observed) = setup(2);
    let c0 = contact(0, 0);
    let mut c1 = contact(1, 1);
    c1.id = c0.id;
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(c0), Transition::Down(c1)],
        InvalidTransition::DuplicateIdentity,
    );
    invalid(
        &mut adapter,
        &observed,
        &[
            Transition::Down(contact(0, 1)),
            Transition::Down(contact(1, 0)),
        ],
        InvalidTransition::TrackingIdNotIncreasing,
    );
    adapter.submit(&[Transition::Down(c0)]).unwrap();
    invalid(
        &mut adapter,
        &observed,
        &[up(c0), Transition::Down(c1)],
        InvalidTransition::DuplicateIdentity,
    );
}

#[test]
fn every_partial_down_write_and_sync_failure_latches_fault_and_recovers_all_slots() {
    // BTN_TOUCH, SLOT, TRACKING_ID, X, Y, ABS_X, ABS_Y, SYN_REPORT.
    for fail_before in 0..8 {
        let (mut adapter, observed) = setup(3);
        let c = contact(2, 0);
        observed.borrow_mut().fail_before = Some(fail_before);
        assert!(
            matches!(adapter.submit(&[Transition::Down(c)]), Err(AdapterError::Transport(e))
            if e.kind() == io::ErrorKind::BrokenPipe)
        );
        assert_eq!(adapter.state(), DeviceState::Faulted);
        assert_eq!(adapter.active_count(), None);
        assert_eq!(observed.borrow().accepted_prefix.len(), fail_before);
        assert!(observed.borrow().frames.is_empty());
        assert!(matches!(adapter.submit(&[]), Err(AdapterError::Faulted)));
        assert!(matches!(
            adapter.submit(&[up(c)]),
            Err(AdapterError::Faulted)
        ));
        assert!(matches!(adapter.release_all(), Err(AdapterError::Faulted)));
        assert_eq!(observed.borrow().attempted.len(), 1);
        adapter.resynchronize().unwrap();
        assert_eq!(adapter.active_count(), Some(0));
        assert_eq!(adapter.state(), DeviceState::Ready);
        assert_eq!(observed.borrow().tracking, vec![-1; 3]);
        assert!(!observed.borrow().touch);
        assert_eq!(
            observed.borrow().frames[0],
            vec![
                touch(false),
                abs(Axis::ABS_MT_SLOT, 0),
                abs(Axis::ABS_MT_TRACKING_ID, -1),
                abs(Axis::ABS_MT_SLOT, 1),
                abs(Axis::ABS_MT_TRACKING_ID, -1),
                abs(Axis::ABS_MT_SLOT, 2),
                abs(Axis::ABS_MT_TRACKING_ID, -1),
                sync(),
            ]
        );
        invalid(
            &mut adapter,
            &observed,
            &[Transition::Down(c)],
            InvalidTransition::TrackingIdNotIncreasing,
        );
        adapter.submit(&[Transition::Down(contact(0, 1))]).unwrap();
        assert_eq!(adapter.active_count(), Some(1));
    }
}

#[test]
fn failed_release_never_reports_a_successfully_cleared_contact() {
    for fail_before in 0..4 {
        let (mut adapter, observed) = setup(2);
        let c = contact(1, 0);
        adapter.submit(&[Transition::Down(c)]).unwrap();
        observed.borrow_mut().fail_before = Some(fail_before);
        assert!(matches!(
            adapter.submit(&[up(c)]),
            Err(AdapterError::Transport(_))
        ));
        assert_eq!(adapter.active_count(), None);
        assert_eq!(adapter.state(), DeviceState::Faulted);
        // Failed cleanup is still unknown, even if every release body event arrived.
        observed.borrow_mut().fail_before = Some(5);
        assert!(matches!(
            adapter.resynchronize(),
            Err(AdapterError::Transport(_))
        ));
        assert_eq!(adapter.active_count(), None);
        assert_eq!(adapter.state(), DeviceState::Faulted);
        adapter.resynchronize().unwrap();
        assert_eq!(adapter.active_count(), Some(0));
    }
}

#[test]
fn failed_move_can_only_resume_after_full_cleanup_and_new_contact() {
    let (mut adapter, observed) = setup(2);
    let c = contact(0, 0);
    adapter.submit(&[Transition::Down(c)]).unwrap();
    let mut moved = c;
    moved.position = PixelPoint { x: 800, y: 1000 };
    observed.borrow_mut().fail_before = Some(3);
    assert!(matches!(
        adapter.submit(&[Transition::Move(moved)]),
        Err(AdapterError::Transport(_))
    ));
    adapter.resynchronize().unwrap();
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Move(moved)],
        InvalidTransition::UnknownContact,
    );
    invalid(
        &mut adapter,
        &observed,
        &[up(c)],
        InvalidTransition::UnknownContact,
    );
    adapter.submit(&[Transition::Down(contact(0, 1))]).unwrap();
}

#[test]
fn release_all_is_idempotent_and_its_failure_is_observable() {
    let (mut adapter, observed) = setup(2);
    adapter.release_all().unwrap();
    adapter.submit(&[]).unwrap();
    assert!(observed.borrow().attempted.is_empty());
    adapter.submit(&[Transition::Down(contact(1, 0))]).unwrap();
    observed.borrow_mut().fail_before = Some(3);
    assert!(matches!(
        adapter.release_all(),
        Err(AdapterError::Transport(_))
    ));
    assert_eq!(adapter.active_count(), None);
    adapter.resynchronize().unwrap();
    let frames = observed.borrow().frames.len();
    adapter.release_all().unwrap();
    assert_eq!(observed.borrow().frames.len(), frames);
}

#[test]
fn explicit_close_drops_transport_on_both_success_and_failure_without_hiding_errors() {
    let (mut adapter, observed) = setup(2);
    adapter.submit(&[Transition::Down(contact(0, 0))]).unwrap();
    adapter.close().unwrap();
    assert!(observed.borrow().dropped);
    assert_eq!(observed.borrow().tracking, vec![-1; 2]);
    let (mut adapter, observed) = setup(2);
    adapter.submit(&[Transition::Down(contact(0, 0))]).unwrap();
    observed.borrow_mut().fail_before = Some(0);
    assert!(matches!(adapter.close(), Err(AdapterError::Transport(_))));
    assert!(observed.borrow().dropped);
    // There is no fiction that failed cleanup cleared this contact.
    assert_eq!(observed.borrow().tracking, vec![0, -1]);
}

#[test]
fn faulted_close_attempts_all_slots_and_drop_does_no_hidden_transport_writes() {
    let (mut adapter, observed) = setup(2);
    observed.borrow_mut().fail_before = Some(4);
    assert!(adapter.submit(&[Transition::Down(contact(1, 0))]).is_err());
    adapter.close().unwrap();
    assert!(observed.borrow().dropped);
    assert_eq!(observed.borrow().tracking, vec![-1; 2]);
    let (mut adapter, observed) = setup(2);
    adapter.submit(&[Transition::Down(contact(0, 0))]).unwrap();
    drop(adapter);
    assert!(observed.borrow().dropped);
    assert_eq!(observed.borrow().attempted.len(), 1);
}

#[test]
fn slot_coordinate_and_tracking_limits_are_exact() {
    let size = AndroidSize::new(1, 1).unwrap();
    assert_eq!(
        DeviceConfig::new(size, 0),
        Err(InvalidTransition::InvalidSlotCount)
    );
    assert_eq!(
        DeviceConfig::new(size, MAX_SLOTS + 1),
        Err(InvalidTransition::InvalidSlotCount)
    );
    assert_eq!(
        DeviceConfig::new(size, MAX_SLOTS).unwrap().slots(),
        MAX_SLOTS
    );
    let (mut adapter, observed) = setup(MAX_SLOTS);
    let mut c = contact(MAX_SLOTS - 1, MAX_TRACKING_ID);
    c.position = PixelPoint { x: 1079, y: 1919 };
    adapter.submit(&[Transition::Down(c)]).unwrap();
    assert_eq!(observed.borrow().tracking[MAX_SLOTS - 1], 0);
    adapter.submit(&[up(c)]).unwrap();
    invalid(
        &mut adapter,
        &observed,
        &[Transition::Down(contact(0, 0))],
        InvalidTransition::TrackingIdNotIncreasing,
    );
}

#[test]
fn repeated_max_capacity_lifecycles_match_mock_device_state() {
    let (mut adapter, observed) = setup(MAX_SLOTS);
    for cycle in 0..128_u32 {
        let contacts: Vec<_> = (0..MAX_SLOTS)
            .map(|slot| contact(slot, cycle * MAX_SLOTS as u32 + slot as u32))
            .collect();
        let downs: Vec<_> = contacts.iter().copied().map(Transition::Down).collect();
        adapter.submit(&downs).unwrap();
        assert_eq!(adapter.active_count(), Some(MAX_SLOTS));
        assert!(observed.borrow().touch);
        for c in &contacts {
            assert_eq!(observed.borrow().tracking[c.slot], c.tracking_id as i32);
        }
        adapter.release_all().unwrap();
        assert_eq!(adapter.active_count(), Some(0));
        assert_eq!(observed.borrow().tracking, vec![-1; MAX_SLOTS]);
        assert!(!observed.borrow().touch);
    }
    for frame in &observed.borrow().frames {
        assert_eq!(frame.iter().filter(|event| **event == sync()).count(), 1);
        assert_eq!(frame.last(), Some(&sync()));
        assert_eq!(frame[0].0, EventType::KEY.0);
    }
}

#[test]
fn wire_id_wrap_keeps_a_long_lived_contact_distinct_and_core_ids_monotonic() {
    let (mut adapter, observed) = setup(2);
    let held = contact(0, 0);
    adapter.submit(&[Transition::Down(held)]).unwrap();
    for id in 1..=65_538 {
        let transient = contact(1, id);
        adapter.submit(&[Transition::Down(transient)]).unwrap();
        let ids = observed.borrow().tracking.clone();
        assert_eq!(ids[0], 0);
        assert!((1..=65_535).contains(&ids[1]));
        assert_ne!(ids[0], ids[1]);
        adapter.submit(&[up(transient)]).unwrap();
        // The mock need not retain a whole long-running session's output history.
        let mut state = observed.borrow_mut();
        state.frames.clear();
        state.attempted.clear();
        state.accepted_prefix.clear();
    }
    assert_eq!(adapter.active_count(), Some(1));
    adapter.submit(&[up(held)]).unwrap();
    assert_eq!(adapter.active_count(), Some(0));
}
