use std::collections::{HashMap, HashSet};

use rex_input_core::{
    AndroidSize, BoundsPolicy, ConfigError, Contact, ContactAllocator, ContactId,
    ContactTransition, CoordinateMapping, InputError, MappingError, PixelPoint, ReleaseReason,
    Rotation, Viewport, MAX_SLOTS,
};

fn mapping(bounds: BoundsPolicy) -> CoordinateMapping {
    CoordinateMapping::new(
        Viewport::new(0.0, 0.0, 100.0, 100.0).unwrap(),
        AndroidSize::new(101, 201).unwrap(),
        Rotation::None,
        bounds,
    )
}

fn allocator(capacity: usize) -> ContactAllocator {
    ContactAllocator::new(capacity, mapping(BoundsPolicy::Reject)).unwrap()
}

fn down(event: ContactTransition) -> Contact {
    match event {
        ContactTransition::Down(contact) => contact,
        other => panic!("expected down, got {other:?}"),
    }
}

#[test]
fn down_move_up_preserve_identity_slot_and_tracking_id() {
    let mut input = allocator(2);
    let initial = down(input.down(ContactId(42), 10.0, 20.0).unwrap());
    assert_eq!(
        initial,
        Contact {
            id: ContactId(42),
            slot: 0,
            tracking_id: 0,
            position: PixelPoint { x: 10, y: 40 }
        }
    );
    let moved = Contact {
        position: PixelPoint { x: 80, y: 180 },
        ..initial
    };
    assert_eq!(
        input.move_to(ContactId(42), 80.0, 90.0),
        Ok(Some(ContactTransition::Move(moved)))
    );
    assert_eq!(input.contact(ContactId(42)), Some(moved));
    assert_eq!(input.move_to(ContactId(42), 80.0, 90.0), Ok(None));
    assert_eq!(
        input.up(ContactId(42)),
        Some(ContactTransition::Up {
            contact: moved,
            reason: ReleaseReason::Up
        })
    );
    assert!(input.up(ContactId(42)).is_none());
    assert_eq!(input.active_count(), 0);
    assert_eq!(
        input.move_to(ContactId(42), 50.0, 50.0),
        Err(InputError::UnknownContact(ContactId(42)))
    );
}

#[test]
fn repeated_down_is_an_error_and_does_not_overwrite_or_leak() {
    let mut input = allocator(2);
    let initial = down(input.down(ContactId(1), 10.0, 20.0).unwrap());
    for (x, y) in [(90.0, 80.0), (f64::NAN, f64::INFINITY)] {
        assert_eq!(
            input.down(ContactId(1), x, y),
            Err(InputError::AlreadyActive(ContactId(1)))
        );
        assert_eq!(input.contacts().collect::<Vec<_>>(), vec![initial]);
    }
    let second = down(input.down(ContactId(2), 20.0, 30.0).unwrap());
    assert_eq!(second.tracking_id, 1);
    assert_eq!(second.slot, 1);
    assert_eq!(input.cancel_all().len(), 2);
}

#[test]
fn errors_leave_contacts_and_tracking_sequence_unchanged() {
    let mut input = allocator(2);
    assert_eq!(
        input.down(ContactId(1), -1.0, 20.0),
        Err(InputError::Mapping(MappingError::OutsideViewport))
    );
    let initial = down(input.down(ContactId(1), 10.0, 20.0).unwrap());
    assert_eq!(initial.tracking_id, 0);
    for (x, y, error) in [
        (-1.0, 20.0, MappingError::OutsideViewport),
        (20.0, 101.0, MappingError::OutsideViewport),
        (f64::NAN, 20.0, MappingError::NonFinitePoint),
        (20.0, f64::INFINITY, MappingError::NonFinitePoint),
    ] {
        assert_eq!(
            input.move_to(ContactId(1), x, y),
            Err(InputError::Mapping(error))
        );
        assert_eq!(input.contact(ContactId(1)), Some(initial));
        assert_eq!(
            input.down(ContactId(2), x, y),
            Err(InputError::Mapping(error))
        );
        assert_eq!(input.active_count(), 1);
    }
    assert_eq!(
        down(input.down(ContactId(2), 50.0, 50.0).unwrap()).tracking_id,
        1
    );
    assert_eq!(input.focus_lost().len(), 2);
    assert_eq!(input.active_count(), 0);
}

#[test]
fn full_capacity_never_evicts_and_lowest_free_slot_is_reused() {
    let mut input = allocator(3);
    let a = down(input.down(ContactId(1), 0.0, 0.0).unwrap());
    let b = down(input.down(ContactId(2), 0.0, 0.0).unwrap());
    let c = down(input.down(ContactId(3), 0.0, 0.0).unwrap());
    assert_eq!(
        input.down(ContactId(4), 0.0, 0.0),
        Err(InputError::CapacityExceeded)
    );
    assert_eq!(input.contacts().collect::<Vec<_>>(), vec![a, b, c]);
    assert!(input.up(ContactId(2)).is_some());
    let d = down(input.down(ContactId(4), 0.0, 0.0).unwrap());
    assert_eq!(d.slot, b.slot);
    assert_eq!(d.tracking_id, 3);
    assert_eq!(input.contacts().collect::<Vec<_>>(), vec![a, d, c]);
    assert_eq!(input.cancel_all().len(), 3);
}

#[test]
fn capacity_bounds_are_checked_and_all_supported_slots_work() {
    for bad in [0, MAX_SLOTS + 1, usize::MAX] {
        assert!(matches!(
            ContactAllocator::new(bad, mapping(BoundsPolicy::Reject)),
            Err(ConfigError::InvalidSlotCount)
        ));
    }
    for capacity in 1..=MAX_SLOTS {
        let mut input = allocator(capacity);
        assert_eq!(input.capacity(), capacity);
        for id in 0..capacity {
            assert_eq!(
                down(input.down(ContactId(id as u64), 0.0, 0.0).unwrap()).slot,
                id
            );
        }
        assert_eq!(
            input.down(ContactId(u64::MAX), 0.0, 0.0),
            Err(InputError::CapacityExceeded)
        );
        assert_eq!(input.focus_lost().len(), capacity);
        assert_eq!(input.active_count(), 0);
    }
}

#[test]
fn cleanup_is_complete_ordered_and_repeatable() {
    let mut input = allocator(3);
    let contacts: Vec<_> = [10, 20, 30]
        .into_iter()
        .map(|id| down(input.down(ContactId(id), id as f64, 0.0).unwrap()))
        .collect();
    let releases = input.cancel_all();
    assert_eq!(
        releases,
        contacts
            .iter()
            .map(|&contact| ContactTransition::Up {
                contact,
                reason: ReleaseReason::Cancelled
            })
            .collect::<Vec<_>>()
    );
    assert_eq!(input.active_count(), 0);
    assert!(input.cancel_all().is_empty());
    assert!(input.focus_lost().is_empty());
    assert!(input.up(ContactId(20)).is_none());
    let restarted = down(input.down(ContactId(20), 10.0, 10.0).unwrap());
    assert_eq!(restarted.slot, 0);
    assert_eq!(restarted.tracking_id, 3);
    assert_eq!(
        input.focus_lost(),
        vec![ContactTransition::Up {
            contact: restarted,
            reason: ReleaseReason::FocusLost
        }]
    );
}

#[test]
fn remapping_releases_old_contacts_before_new_geometry_is_used() {
    let mut input = allocator(1);
    let old = down(input.down(ContactId(1), 100.0, 0.0).unwrap());
    let replacement = CoordinateMapping::new(
        Viewport::new(10.0, 10.0, 20.0, 40.0).unwrap(),
        AndroidSize::new(50, 80).unwrap(),
        Rotation::Clockwise90,
        BoundsPolicy::Reject,
    );
    assert_eq!(
        input.set_mapping(replacement),
        vec![ContactTransition::Up {
            contact: old,
            reason: ReleaseReason::MappingChanged
        }]
    );
    assert_eq!(input.active_count(), 0);
    assert!(input.up(ContactId(1)).is_none());
    let new = down(input.down(ContactId(1), 10.0, 10.0).unwrap());
    assert_eq!(new.position, PixelPoint { x: 49, y: 0 });
    assert_eq!(new.tracking_id, 1);
    assert_eq!(input.set_mapping(replacement).len(), 1);
    assert!(input.set_mapping(replacement).is_empty());
}

#[test]
fn clamp_policy_cannot_create_out_of_range_contacts() {
    let mut input = ContactAllocator::new(1, mapping(BoundsPolicy::Clamp)).unwrap();
    let contact = down(input.down(ContactId(1), -f64::MAX, f64::MAX).unwrap());
    assert_eq!(contact.position, PixelPoint { x: 0, y: 200 });
    assert_eq!(
        input.move_to(ContactId(1), f64::MAX, -f64::MAX),
        Ok(Some(ContactTransition::Move(Contact {
            position: PixelPoint { x: 100, y: 0 },
            ..contact
        })))
    );
    assert_eq!(input.cancel_all().len(), 1);
}

// A deliberately small in-memory adapter checks event/state consistency; no OS I/O.
fn replay(live: &mut HashMap<usize, Contact>, event: ContactTransition) {
    match event {
        ContactTransition::Down(contact) => assert!(live.insert(contact.slot, contact).is_none()),
        ContactTransition::Move(contact) => {
            let previous = live.get(&contact.slot).expect("move must follow down");
            assert_eq!(previous.id, contact.id);
            assert_eq!(previous.tracking_id, contact.tracking_id);
            live.insert(contact.slot, contact);
        }
        ContactTransition::Up { contact, .. } => {
            assert_eq!(live.remove(&contact.slot), Some(contact))
        }
    }
}

fn assert_consistent(input: &ContactAllocator, live: &HashMap<usize, Contact>) {
    assert_eq!(input.active_count(), live.len());
    assert!(live.len() <= input.capacity());
    let mut ids = HashSet::new();
    let mut tracking = HashSet::new();
    for contact in input.contacts() {
        assert_eq!(live.get(&contact.slot), Some(&contact));
        assert!(contact.slot < input.capacity());
        assert!(ids.insert(contact.id));
        assert!(tracking.insert(contact.tracking_id));
        assert!(contact.position.x <= 100 && contact.position.y <= 200);
    }
}

#[test]
fn five_hundred_twelve_interrupted_lifecycles_leave_no_contacts() {
    let mut input = allocator(10);
    let mut live = HashMap::new();
    let mut seen = HashSet::new();
    for cycle in 0..512 {
        for id in 0..10 {
            let event = input.down(ContactId(id), 10.0, 20.0).unwrap();
            assert!(seen.insert(down(event).tracking_id));
            replay(&mut live, event);
            assert_eq!(
                input.down(ContactId(id), 90.0, 90.0),
                Err(InputError::AlreadyActive(ContactId(id)))
            );
            replay(
                &mut live,
                input.move_to(ContactId(id), 80.0, 90.0).unwrap().unwrap(),
            );
        }
        assert_consistent(&input, &live);
        assert_eq!(
            input.down(ContactId(999), 0.0, 0.0),
            Err(InputError::CapacityExceeded)
        );
        for id in [0, 3, 9] {
            replay(&mut live, input.up(ContactId(id)).unwrap());
            assert!(input.up(ContactId(id)).is_none());
        }
        let releases = match cycle % 3 {
            0 => input.cancel_all(),
            1 => input.focus_lost(),
            _ => input.set_mapping(mapping(BoundsPolicy::Reject)),
        };
        assert_eq!(releases.len(), 7);
        for event in releases {
            replay(&mut live, event);
        }
        assert_consistent(&input, &live);
        assert!(live.is_empty());
        assert!(input.focus_lost().is_empty());
        assert!(input.cancel_all().is_empty());
    }
    assert_eq!(seen.len(), 5120);
}

#[test]
fn twenty_thousand_deterministic_mixed_operations_preserve_state() {
    let mut input = allocator(5);
    let mut live = HashMap::new();
    let mut random = 0x1234_5678u64;
    let mut seen = HashSet::new();
    for _ in 0..20_000 {
        random = random.wrapping_mul(6364136223846793005).wrapping_add(1);
        let id = ContactId((random >> 32) % 8);
        let x = ((random >> 40) % 120) as f64;
        let y = ((random >> 48) % 120) as f64;
        let before = input.contacts().collect::<Vec<_>>();
        match (random >> 24) % 7 {
            0 | 1 => match input.down(id, x, y) {
                Ok(event) => {
                    assert!(seen.insert(down(event).tracking_id));
                    replay(&mut live, event);
                }
                Err(_) => assert_eq!(input.contacts().collect::<Vec<_>>(), before),
            },
            2 | 3 => match input.move_to(id, x, y) {
                Ok(Some(event)) => replay(&mut live, event),
                Ok(None) | Err(_) => assert_eq!(input.contacts().collect::<Vec<_>>(), before),
            },
            4 => {
                if let Some(event) = input.up(id) {
                    replay(&mut live, event);
                }
            }
            5 => {
                for event in input.cancel_all() {
                    replay(&mut live, event);
                }
            }
            _ => {
                for event in input.focus_lost() {
                    replay(&mut live, event);
                }
            }
        }
        assert_consistent(&input, &live);
    }
    for event in input.cancel_all() {
        replay(&mut live, event);
    }
    assert!(live.is_empty());
    assert_eq!(input.active_count(), 0);
}
