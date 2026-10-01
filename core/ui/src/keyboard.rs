//! Fail-closed logical-key bridge for GPUI 0.2.2. Physical scancodes are not
//! exposed consistently, so modifier/layout changes release and disable input.
use rex_keymap::{Binding, HostEvent, HostKey, Keymap, Trigger};
use std::collections::HashSet;

#[derive(Debug, PartialEq)]
pub enum KeyDecision {
    Ignore,
    Event(HostEvent),
    ReleaseAll,
}
#[derive(Default)]
pub struct NativeKeyboard {
    held: HashSet<HostKey>,
}
impl NativeKeyboard {
    pub fn clear(&mut self) {
        self.held.clear();
    }
    pub fn press(&mut self, raw: &str, repeat: bool, modified: bool) -> KeyDecision {
        if modified {
            self.clear();
            return KeyDecision::ReleaseAll;
        }
        if repeat {
            return KeyDecision::Ignore;
        }
        let Ok(key) = HostKey::new(raw) else {
            return KeyDecision::Ignore;
        };
        if !self.held.insert(key.clone()) {
            return KeyDecision::Ignore;
        }
        KeyDecision::Event(HostEvent::KeyDown { key, repeat: false })
    }
    pub fn release(&mut self, raw: &str) -> KeyDecision {
        if let Ok(key) = HostKey::new(raw)
            && self.held.remove(&key)
        {
            return KeyDecision::Event(HostEvent::KeyUp { key });
        }
        if self.held.is_empty() {
            KeyDecision::Ignore
        } else {
            self.clear();
            KeyDecision::ReleaseAll
        }
    }
    pub fn modifiers_or_layout_changed(&mut self) -> KeyDecision {
        self.clear();
        KeyDecision::ReleaseAll
    }
}

/// GPUI delivers modifiers separately and may change logical release symbols.
/// Reject these native profiles explicitly rather than silently not activating them.
pub fn validate_native_keymap(keymap: &Keymap) -> Result<(), String> {
    let reserved = |key: &HostKey| matches!(key.as_str(), "shift" | "control" | "alt" | "escape");
    for binding in keymap.bindings() {
        let unsupported = match binding {
            Binding::Tap {
                trigger: Trigger::Key { key },
                ..
            }
            | Binding::Hold {
                trigger: Trigger::Key { key },
                ..
            } => reserved(key),
            Binding::Joystick {
                up,
                left,
                down,
                right,
                ..
            } => [up, left, down, right].iter().any(|key| reserved(key)),
            _ => false,
        };
        if unsupported {
            return Err("Native GPUI input currently reserves shift/control/alt/escape for safe release. Use unmodified non-reserved keys in this profile; modifier or keyboard-layout changes disable input.".into());
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn changed_release_symbol_cannot_leave_a_contact_held() {
        let mut keyboard = NativeKeyboard::default();
        assert!(matches!(
            keyboard.press("2", false, false),
            KeyDecision::Event(_)
        ));
        assert_eq!(keyboard.release("@"), KeyDecision::ReleaseAll);
        assert_eq!(keyboard.release("2"), KeyDecision::Ignore);
    }
    #[test]
    fn modifier_change_releases_and_clears_before_shifted_key_up() {
        let mut keyboard = NativeKeyboard::default();
        keyboard.press("2", false, false);
        assert_eq!(
            keyboard.modifiers_or_layout_changed(),
            KeyDecision::ReleaseAll
        );
        assert_eq!(keyboard.release("@"), KeyDecision::Ignore);
    }
    #[test]
    fn repeat_and_duplicate_down_do_not_retrigger() {
        let mut keyboard = NativeKeyboard::default();
        keyboard.press("w", false, false);
        assert_eq!(keyboard.press("w", true, false), KeyDecision::Ignore);
        assert_eq!(keyboard.press("w", false, false), KeyDecision::Ignore);
        assert!(matches!(
            keyboard.release("w"),
            KeyDecision::Event(HostEvent::KeyUp { .. })
        ));
    }
    #[test]
    fn native_profile_rejects_modifier_trigger_with_actionable_error() {
        let map = Keymap::from_json(r#"{"version":1,"name":"modifier","pointer":false,"bindings":[{"kind":"hold","trigger":{"kind":"key","key":"shift"},"point":{"x":0.5,"y":0.5}}]}"#).unwrap();
        assert!(
            validate_native_keymap(&map)
                .unwrap_err()
                .contains("modifier")
        );
        assert!(validate_native_keymap(&Keymap::default()).is_ok());
    }
}
