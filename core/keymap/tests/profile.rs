use rex_keymap::{HostKey, Keymap, KeymapError, MAX_PROFILE_BYTES};
use serde_json::{json, Value};

fn profile() -> Value {
    json!({"version":1,"name":"Test","pointer":false,"bindings":[
        {"kind":"hold","trigger":{"kind":"key","key":"space"},"point":{"x":0.8,"y":0.7}}
    ]})
}
fn rejects(value: Value) {
    assert!(
        Keymap::from_json(&value.to_string()).is_err(),
        "accepted {value}"
    );
}

#[test]
fn default_profile_round_trips_and_is_bounded() {
    let map = Keymap::default();
    assert_eq!(map.max_contacts(), 4);
    assert!(map.pointer_enabled());
    let json = map.to_json().unwrap();
    assert_eq!(Keymap::from_json(&json).unwrap().to_json().unwrap(), json);
}

#[test]
fn rejects_unknown_duplicate_missing_and_unversioned_fields() {
    let mut value = profile();
    value["version"] = json!(2);
    rejects(value);
    let mut value = profile();
    value["unknown"] = json!(true);
    rejects(value);
    let mut value = profile();
    value.as_object_mut().unwrap().remove("version");
    rejects(value);
    let mut value = profile();
    value["bindings"][0]["unknown"] = json!(true);
    rejects(value);
    let mut value = profile();
    value["bindings"][0]["trigger"]["unknown"] = json!(true);
    rejects(value);
    let mut value = profile();
    value["bindings"][0]["point"]["unknown"] = json!(true);
    rejects(value);
    assert!(Keymap::from_json(
        r#"{"version":1,"version":1,"name":"X","pointer":true,"bindings":[]}"#
    )
    .is_err());
    assert!(Keymap::from_json(r#"{"version":1,"name":"X","pointer":false,"bindings":[{"kind":"hold","trigger":{"kind":"key","key":"x"},"point":{"x":0.5,"x":0.6,"y":0.5}}]}"#).is_err());
}

#[test]
fn rejects_ambiguous_triggers_reserved_escape_and_pointer_collision() {
    let mut value = profile();
    let same = value["bindings"][0].clone();
    value["bindings"].as_array_mut().unwrap().push(same);
    rejects(value);
    let mut value = profile();
    value["bindings"][0]["trigger"]["key"] = json!("escape");
    rejects(value);
    let mut value = profile();
    value["pointer"] = json!(true);
    value["bindings"][0]["trigger"] = json!({"kind":"mouse","button":"left"});
    rejects(value);
    let value = json!({"version":1,"name":"X","bindings":[{"kind":"joystick","up":"w","left":"a","down":"w","right":"d","center":{"x":0.5,"y":0.5},"radius":0.2}]});
    rejects(value);
}

#[test]
fn validates_coordinates_radius_names_and_resource_limits() {
    for point in [
        json!({"x":-0.1,"y":0.5}),
        json!({"x":0.5,"y":1.1}),
        json!({"x":"NaN","y":0.5}),
    ] {
        let mut value = profile();
        value["bindings"][0]["point"] = point;
        rejects(value);
    }
    for radius in [0.0, -1.0, 0.5001] {
        rejects(
            json!({"version":1,"name":"X","bindings":[{"kind":"joystick","up":"w","left":"a","down":"s","right":"d","center":{"x":0.5,"y":0.5},"radius":radius}]}),
        );
    }
    for name in ["".to_owned(), "x".repeat(81), "line\nbreak".to_owned()] {
        let mut value = profile();
        value["name"] = json!(name);
        rejects(value);
    }
    rejects(json!({"version":1,"name":"X","bindings":[]}));
    let mut value = profile();
    value["bindings"] = json!(vec![value["bindings"][0].clone(); 33]);
    rejects(value);
    assert!(matches!(
        Keymap::from_json(&" ".repeat(MAX_PROFILE_BYTES + 1)),
        Err(KeymapError::ProfileTooLarge)
    ));
    assert!(Keymap::from_json(&"[".repeat(1024)).is_err());
}

#[test]
fn key_vocabulary_is_canonical_and_bounded() {
    for key in ["A", "0", "f1", "f24", "space", "enter", "escape"] {
        assert_eq!(
            HostKey::new(key).unwrap().as_str(),
            key.to_ascii_lowercase()
        );
    }
    for key in [
        "",
        " ",
        "é",
        "f0",
        "f01",
        "f25",
        "ctrl+x",
        "arbitrary-key-name",
        "Space ",
    ] {
        assert!(HostKey::new(key).is_err(), "accepted {key}");
    }
}

#[test]
fn reader_stops_at_limit_and_rejects_invalid_utf8_or_io_errors() {
    use std::io::{self, Read};
    struct Endless {
        read: usize,
    }
    impl Read for Endless {
        fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
            bytes.fill(b' ');
            self.read += bytes.len();
            Ok(bytes.len())
        }
    }
    let mut reader = Endless { read: 0 };
    assert!(matches!(
        Keymap::from_reader(&mut reader),
        Err(KeymapError::ProfileTooLarge)
    ));
    assert_eq!(reader.read, MAX_PROFILE_BYTES + 1);
    assert!(Keymap::from_reader(&[0xff][..]).is_err());
    assert_eq!(
        Keymap::from_reader(Keymap::default().to_json().unwrap().as_bytes())
            .unwrap()
            .max_contacts(),
        4
    );
    struct Broken;
    impl Read for Broken {
        fn read(&mut self, _bytes: &mut [u8]) -> io::Result<usize> {
            Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "test read failure",
            ))
        }
    }
    assert!(matches!(
        Keymap::from_reader(Broken),
        Err(KeymapError::Read(_))
    ));
}
