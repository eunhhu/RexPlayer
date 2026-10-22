//! Never drop a release merely because a status snapshot is unavailable.
//! The worker's send gate is authoritative; losing a prerequisite closes it.
use rex_keymap::HostEvent;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputGate {
    Inactive,
    Deliver,
    Close,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dispatch {
    Ignored,
    Delivered,
    Closed,
}

pub fn window_gate(
    armed: bool,
    active: bool,
    fresh: bool,
    modal: bool,
    viewport_matches: bool,
) -> InputGate {
    if !armed {
        InputGate::Inactive
    } else if active && fresh && !modal && viewport_matches {
        InputGate::Deliver
    } else {
        InputGate::Close
    }
}

/// Delivers once without consulting a racy readiness snapshot, or closes input.
/// A failed enqueue is never retried and always requests out-of-band cleanup.
pub fn dispatch<E>(
    gate: InputGate,
    event: HostEvent,
    send: impl FnOnce(HostEvent) -> Result<(), E>,
    close: impl FnOnce(),
) -> Result<Dispatch, E> {
    match gate {
        InputGate::Inactive => Ok(Dispatch::Ignored),
        InputGate::Close => {
            close();
            Ok(Dispatch::Closed)
        }
        InputGate::Deliver => match send(event) {
            Ok(()) => Ok(Dispatch::Delivered),
            Err(error) => {
                close();
                Err(error)
            }
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rex_keymap::HostKey;
    use std::cell::Cell;
    fn release() -> HostEvent {
        HostEvent::KeyUp {
            key: HostKey::new("w").unwrap(),
        }
    }
    #[test]
    fn release_is_enqueued_even_without_a_readiness_snapshot() {
        let sent = Cell::new(false);
        let closed = Cell::new(false);
        // No readiness/status option exists in the gate; try-lock contention
        // cannot silently suppress this release before the authoritative send.
        let outcome = dispatch(
            window_gate(true, true, true, false, true),
            release(),
            |event| {
                assert_eq!(event, release());
                sent.set(true);
                Ok::<_, ()>(())
            },
            || closed.set(true),
        );
        assert_eq!(outcome, Ok(Dispatch::Delivered));
        assert!(sent.get());
        assert!(!closed.get());
    }
    #[test]
    fn stale_release_closes_immediately_before_a_new_frame_can_arrive() {
        let closed = Cell::new(false);
        let outcome = dispatch(
            window_gate(true, true, false, false, true),
            release(),
            |_| -> Result<(), ()> { panic!("stale event must not be sent") },
            || closed.set(true),
        );
        assert_eq!(outcome, Ok(Dispatch::Closed));
        assert!(closed.get());
    }
    #[test]
    fn every_lost_prerequisite_or_failed_enqueue_closes() {
        for gate in [
            window_gate(true, false, true, false, true),
            window_gate(true, true, true, true, true),
            window_gate(true, true, true, false, false),
        ] {
            assert_eq!(gate, InputGate::Close);
        }
        let closed = Cell::new(false);
        assert_eq!(
            dispatch(
                InputGate::Deliver,
                release(),
                |_| Err("full"),
                || closed.set(true)
            ),
            Err("full")
        );
        assert!(closed.get());
    }
}
