use rex_input_core::{
    AndroidSize, ContactTransition as Transition, PixelPoint, ReleaseReason, Rotation,
};
use rex_keymap::{HostEvent, HostKey, Keymap, KeymapController, MouseButton, WindowViewport};

fn key(value: &str) -> HostKey {
    HostKey::new(value).unwrap()
}
fn down(value: &str) -> HostEvent {
    HostEvent::KeyDown {
        key: key(value),
        repeat: false,
    }
}
fn up(value: &str) -> HostEvent {
    HostEvent::KeyUp { key: key(value) }
}
fn controller(rotation: Rotation) -> KeymapController {
    let mut controller = KeymapController::new(
        Keymap::default(),
        WindowViewport::new(100.0, 200.0, 1000.0, 1000.0).unwrap(),
        AndroidSize::new(1001, 1001).unwrap(),
        rotation,
    )
    .unwrap();
    assert!(controller
        .handle(HostEvent::FocusGained)
        .unwrap()
        .is_empty());
    controller
}
fn point(transition: Transition) -> PixelPoint {
    match transition {
        Transition::Down(c) | Transition::Move(c) | Transition::Up { contact: c, .. } => c.position,
    }
}

#[test]
fn starts_unfocused_and_gates_input_after_focus_loss() {
    let mut c = KeymapController::new(
        Keymap::default(),
        WindowViewport::new(0.0, 0.0, 100.0, 100.0).unwrap(),
        AndroidSize::new(100, 100).unwrap(),
        Rotation::None,
    )
    .unwrap();
    assert!(c.handle(down("space")).unwrap().is_empty());
    c.handle(HostEvent::FocusGained).unwrap();
    assert_eq!(c.handle(down("space")).unwrap().len(), 1);
    let released = c.handle(HostEvent::FocusLost).unwrap();
    assert!(matches!(
        released[0],
        Transition::Up {
            reason: ReleaseReason::FocusLost,
            ..
        }
    ));
    assert!(!c.focused());
    assert!(c.handle(down("space")).unwrap().is_empty());
    assert!(c.handle(HostEvent::FocusLost).unwrap().is_empty());
    c.handle(HostEvent::FocusGained).unwrap();
    assert!(c
        .handle(HostEvent::KeyDown {
            key: key("space"),
            repeat: true
        })
        .unwrap()
        .is_empty());
    assert!(c.handle(up("space")).unwrap().is_empty());
    assert_eq!(c.handle(down("space")).unwrap().len(), 1);
}

#[test]
fn hold_repeat_and_tap_have_distinct_lifecycles() {
    let mut c = controller(Rotation::None);
    let events = c.handle(down("space")).unwrap();
    assert_eq!(point(events[0]), PixelPoint { x: 800, y: 750 });
    assert!(c.handle(down("space")).unwrap().is_empty());
    assert!(c
        .handle(HostEvent::KeyDown {
            key: key("space"),
            repeat: true
        })
        .unwrap()
        .is_empty());
    let events = c.handle(down("e")).unwrap();
    assert_eq!(events.len(), 2);
    assert!(
        matches!((events[0], events[1]), (Transition::Down(a),Transition::Up {contact:b,..}) if a == b)
    );
    assert_eq!(c.active_count(), 1);
    assert!(c.handle(down("e")).unwrap().is_empty());
    assert!(c.handle(up("e")).unwrap().is_empty());
    assert_eq!(c.handle(down("e")).unwrap().len(), 2);
    assert_eq!(c.handle(up("space")).unwrap().len(), 1);
    assert!(c.handle(up("space")).unwrap().is_empty());
}

#[test]
fn joystick_starts_at_center_normalizes_diagonals_and_combines_simultaneous_keys() {
    let mut c = controller(Rotation::None);
    let events = c.handle(down("w")).unwrap();
    assert_eq!(events.len(), 2);
    assert_eq!(point(events[0]), PixelPoint { x: 200, y: 750 });
    assert_eq!(point(events[1]), PixelPoint { x: 200, y: 600 });
    let events = c.handle(down("d")).unwrap();
    assert_eq!(point(events[0]), PixelPoint { x: 306, y: 644 });
    let events = c.handle(down("s")).unwrap();
    assert_eq!(point(events[0]), PixelPoint { x: 350, y: 750 });
    let events = c.handle(down("a")).unwrap();
    assert_eq!(point(events[0]), PixelPoint { x: 200, y: 750 });
    assert_eq!(c.active_count(), 1);
    c.handle(up("a")).unwrap();
    c.handle(up("d")).unwrap();
    c.handle(up("w")).unwrap();
    assert_eq!(c.handle(up("s")).unwrap().len(), 1);
    assert_eq!(c.active_count(), 0);
}

#[test]
fn all_rotations_apply_to_mapped_points() {
    for (rotation, expected) in [
        (Rotation::None, PixelPoint { x: 800, y: 750 }),
        (Rotation::Clockwise90, PixelPoint { x: 250, y: 800 }),
        (Rotation::Clockwise180, PixelPoint { x: 200, y: 250 }),
        (Rotation::Clockwise270, PixelPoint { x: 750, y: 200 }),
    ] {
        assert_eq!(
            point(controller(rotation).handle(down("space")).unwrap()[0]),
            expected
        );
    }
}

#[test]
fn direct_pointer_starts_inside_drags_clamped_and_releases_without_coordinates() {
    let mut c = controller(Rotation::None);
    for (x, y) in [(0.0, 0.0), (f64::NAN, 250.0), (150.0, f64::INFINITY)] {
        assert!(c
            .handle(HostEvent::MouseDown {
                button: MouseButton::Left,
                x,
                y
            })
            .unwrap()
            .is_empty());
    }
    let events = c
        .handle(HostEvent::MouseDown {
            button: MouseButton::Left,
            x: 600.0,
            y: 700.0,
        })
        .unwrap();
    assert_eq!(point(events[0]), PixelPoint { x: 500, y: 500 });
    assert!(c
        .handle(HostEvent::MouseDown {
            button: MouseButton::Left,
            x: 800.0,
            y: 900.0
        })
        .unwrap()
        .is_empty());
    assert!(c
        .handle(HostEvent::MouseMove { x: 600.0, y: 700.0 })
        .unwrap()
        .is_empty());
    assert!(c
        .handle(HostEvent::MouseMove {
            x: f64::NAN,
            y: 700.0
        })
        .is_err());
    assert_eq!(c.active_count(), 1);
    let events = c
        .handle(HostEvent::MouseMove {
            x: 9000.0,
            y: -500.0,
        })
        .unwrap();
    assert_eq!(point(events[0]), PixelPoint { x: 1000, y: 0 });
    assert_eq!(
        c.handle(HostEvent::MouseUp {
            button: MouseButton::Left
        })
        .unwrap()
        .len(),
        1
    );
    assert!(c
        .handle(HostEvent::MouseMove { x: 600.0, y: 700.0 })
        .unwrap()
        .is_empty());
}

#[test]
fn resize_and_escape_release_all_owners_at_old_positions() {
    let mut c = controller(Rotation::None);
    c.handle(down("w")).unwrap();
    c.handle(down("space")).unwrap();
    c.handle(HostEvent::MouseDown {
        button: MouseButton::Left,
        x: 600.0,
        y: 700.0,
    })
    .unwrap();
    let events = c.set_viewport(WindowViewport::new(0.0, 0.0, 100.0, 200.0).unwrap());
    assert_eq!(events.len(), 3);
    assert!(events.iter().all(|e| matches!(
        e,
        Transition::Up {
            reason: ReleaseReason::MappingChanged,
            ..
        }
    )));
    assert!(c.focused());
    assert_eq!(c.active_count(), 0);
    assert!(c
        .handle(HostEvent::KeyDown {
            key: key("w"),
            repeat: true
        })
        .unwrap()
        .is_empty());
    assert!(c
        .handle(HostEvent::MouseMove { x: 10.0, y: 10.0 })
        .unwrap()
        .is_empty());
    c.handle(down("space")).unwrap();
    let events = c.handle(down("escape")).unwrap();
    assert!(matches!(
        events[0],
        Transition::Up {
            reason: ReleaseReason::Cancelled,
            ..
        }
    ));
    assert!(!c.focused());
    assert!(c.handle(down("space")).unwrap().is_empty());
}

#[test]
fn mapped_mouse_button_is_independent_of_pointer_and_keyboard() {
    let map=Keymap::from_json(r#"{"version":1,"name":"Mouse","pointer":true,"bindings":[{"kind":"hold","trigger":{"kind":"mouse","button":"right"},"point":{"x":0.25,"y":0.75}}]}"#).unwrap();
    let mut c = KeymapController::new(
        map,
        WindowViewport::new(0.0, 0.0, 100.0, 100.0).unwrap(),
        AndroidSize::new(101, 101).unwrap(),
        Rotation::None,
    )
    .unwrap();
    c.handle(HostEvent::FocusGained).unwrap();
    assert_eq!(
        point(
            c.handle(HostEvent::MouseDown {
                button: MouseButton::Right,
                x: 50.0,
                y: 50.0
            })
            .unwrap()[0]
        ),
        PixelPoint { x: 25, y: 75 }
    );
    c.handle(HostEvent::MouseDown {
        button: MouseButton::Left,
        x: 50.0,
        y: 50.0,
    })
    .unwrap();
    assert_eq!(c.active_count(), 2);
    assert_eq!(
        c.handle(HostEvent::MouseUp {
            button: MouseButton::Right
        })
        .unwrap()
        .len(),
        1
    );
    assert_eq!(c.active_count(), 1);
    assert_eq!(c.handle(HostEvent::EmergencyRelease).unwrap().len(), 1);
}

#[test]
fn maximum_profile_owners_are_all_simultaneously_usable() {
    let keys: Vec<_> = ('a'..='z')
        .chain('0'..='5')
        .map(|c| c.to_string())
        .collect();
    let bindings: Vec<_> = keys.iter().map(|key|serde_json::json!({"kind":"hold","trigger":{"kind":"key","key":key},"point":{"x":0.5,"y":0.5}})).collect();
    let profile = serde_json::json!({"version":1,"name":"Capacity","bindings":bindings});
    let map = Keymap::from_json(&profile.to_string()).unwrap();
    assert_eq!(map.max_contacts(), 32);
    let mut c = KeymapController::new(
        map,
        WindowViewport::new(0.0, 0.0, 100.0, 100.0).unwrap(),
        AndroidSize::new(100, 100).unwrap(),
        Rotation::None,
    )
    .unwrap();
    c.handle(HostEvent::FocusGained).unwrap();
    for key in &keys {
        assert_eq!(c.handle(down(key)).unwrap().len(), 1);
    }
    assert_eq!(c.active_count(), 32);
    assert_eq!(c.handle(HostEvent::EmergencyRelease).unwrap().len(), 32);
    assert_eq!(c.active_count(), 0);
}

#[test]
fn deterministic_mixed_events_preserve_a_strict_independent_consumer() {
    use rex_input_core::Contact;
    let mut c = controller(Rotation::None);
    let mut contacts: Vec<Option<Contact>> = vec![None; c.max_contacts()];
    let mut last_tracking = None;
    let mut seed = 0x1234abcd_u64;
    for index in 0..20_000 {
        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
        let events = match seed % 18 {
            0 => c.handle(down("w")),
            1 => c.handle(down("a")),
            2 => c.handle(down("s")),
            3 => c.handle(down("d")),
            4 => c.handle(up("w")),
            5 => c.handle(up("a")),
            6 => c.handle(up("s")),
            7 => c.handle(up("d")),
            8 => c.handle(down("space")),
            9 => c.handle(up("space")),
            10 => c.handle(down("e")),
            11 => c.handle(up("e")),
            12 => c.handle(HostEvent::MouseDown {
                button: MouseButton::Left,
                x: 600.0,
                y: 700.0,
            }),
            13 => c.handle(HostEvent::MouseMove {
                x: (seed % 2000) as f64,
                y: ((seed >> 12) % 2000) as f64,
            }),
            14 => c.handle(HostEvent::MouseUp {
                button: MouseButton::Left,
            }),
            15 => c.handle(HostEvent::FocusLost),
            16 => c.handle(HostEvent::FocusGained),
            _ => Ok(c.set_geometry(
                WindowViewport::new(100.0, 200.0, 1000.0, 1000.0).unwrap(),
                AndroidSize::new(1001, 1001).unwrap(),
                if index % 2 == 0 {
                    Rotation::None
                } else {
                    Rotation::Clockwise90
                },
            )),
        }
        .unwrap();
        for event in events {
            match event {
                Transition::Down(contact) => {
                    assert!(contacts[contact.slot].is_none());
                    assert!(contacts.iter().flatten().all(|old| old.id != contact.id));
                    assert!(last_tracking.is_none_or(|old| contact.tracking_id > old));
                    assert!(contact.position.x < 1001 && contact.position.y < 1001);
                    last_tracking = Some(contact.tracking_id);
                    contacts[contact.slot] = Some(contact);
                }
                Transition::Move(contact) => {
                    let old = contacts[contact.slot].unwrap();
                    assert_eq!((old.id, old.tracking_id), (contact.id, contact.tracking_id));
                    assert!(contact.position.x < 1001 && contact.position.y < 1001);
                    contacts[contact.slot] = Some(contact);
                }
                Transition::Up { contact, .. } => {
                    assert_eq!(contacts[contact.slot], Some(contact));
                    contacts[contact.slot] = None;
                }
            }
        }
        assert_eq!(contacts.iter().flatten().count(), c.active_count());
    }
    let final_events = c.handle(HostEvent::EmergencyRelease).unwrap();
    assert_eq!(final_events.len(), contacts.iter().flatten().count());
    assert_eq!(c.active_count(), 0);
}
