//! Explicit Linux adapter ownership for a window input controller.
//!
//! Construction with [`LinuxInputSession::create`] opens `/dev/uinput` only when
//! the caller explicitly invokes it. Device creation and successful writes do not
//! prove the Android container can see the device. Runtime device discovery and
//! guest-visible input confirmation are separate readiness checks. No permissions,
//! device routes, namespaces, or security settings are changed here.

use crate::{HostEvent, KeymapController, KeymapError, WindowViewport};
use rex_input_core::{AndroidSize, ContactTransition, Rotation};
use rex_input_linux::{
    AdapterError, DeviceConfig, DeviceState, FrameTransport, LinuxTouchDevice, TouchAdapter,
    UinputTransport,
};
use std::{error::Error, fmt, io};

/// Failure while constructing, translating, or submitting window input.
#[derive(Debug)]
pub enum SessionError {
    /// Real virtual-device creation failed; no automatic permission changes follow.
    Open(io::Error),
    /// Source input or profile validation failed.
    Input(KeymapError),
    /// Adapter submission failed. The session is blocked until resynchronized.
    Adapter(AdapterError),
    /// A supplied adapter/controller has incompatible geometry, capacity, or state.
    IncompatibleAdapter,
    /// Destination dimensions changed; create a correctly sized new device first.
    DestinationChanged,
    /// An earlier adapter error requires explicit cleanup before accepting events.
    Blocked,
}
impl fmt::Display for SessionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Open(error) => write!(f, "could not create RexPlayer virtual touchscreen: {error}"),
            Self::Input(error) => error.fmt(f),
            Self::Adapter(error) => error.fmt(f),
            Self::IncompatibleAdapter => f.write_str("input controller and adapter must be empty, ready, and have matching dimensions and slots"),
            Self::DestinationChanged => f.write_str("Android dimensions changed; close this input session and create a new device"),
            Self::Blocked => f.write_str("input session is blocked; explicitly resynchronize or close it"),
        }
    }
}
impl Error for SessionError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Open(error) => Some(error),
            Self::Input(error) => Some(error),
            Self::Adapter(error) => Some(error),
            _ => None,
        }
    }
}

/// Owns one controller and its exclusive output adapter.
///
/// Each source transition becomes a separate synchronization frame, including
/// tap down/up and joystick center/down then move. An adapter failure immediately
/// clears source state, disables input, and discards the remaining batch. There is
/// no replay or automatic recovery. Call [`Self::close`] before dropping a live
/// session to observe cleanup errors. Dropping alone cannot report release delivery.
#[derive(Debug)]
pub struct LinuxInputSession<T: FrameTransport> {
    controller: KeymapController,
    adapter: TouchAdapter<T>,
    blocked: bool,
}

impl LinuxInputSession<UinputTransport> {
    /// Explicit opt-in to open `/dev/uinput`. Caller must have existing permissions
    /// and independently establish that the intended guest can receive this device.
    /// No event is written here. An already-active source controller is rejected.
    pub fn create(controller: KeymapController) -> Result<Self, SessionError> {
        if controller.active_count() != 0 {
            return Err(SessionError::IncompatibleAdapter);
        }
        let config = DeviceConfig::new(controller.android_size(), controller.max_contacts())
            .map_err(|error| SessionError::Adapter(error.into()))?;
        let adapter = LinuxTouchDevice::create(config).map_err(SessionError::Open)?;
        Self::with_adapter(controller, adapter)
    }

    /// Lists only event nodes belonging to this virtual device. Empty results do
    /// not establish consumer readiness; this does not inspect or open guest devices.
    pub fn device_nodes(&mut self) -> io::Result<Vec<std::path::PathBuf>> {
        self.adapter.device_nodes()
    }
}

impl<T: FrameTransport> LinuxInputSession<T> {
    /// Wraps an empty ready adapter of exactly matching size/capacity, without I/O.
    /// The transport must follow the adapter's exclusive-writer contract. Intended
    /// also for deterministic end-to-end translation tests with an in-memory writer.
    pub fn with_adapter(
        controller: KeymapController,
        adapter: TouchAdapter<T>,
    ) -> Result<Self, SessionError> {
        if controller.active_count() != 0
            || adapter.active_count() != Some(0)
            || adapter.config().size() != controller.android_size()
            || adapter.config().slots() != controller.max_contacts()
        {
            return Err(SessionError::IncompatibleAdapter);
        }
        Ok(Self {
            controller,
            adapter,
            blocked: false,
        })
    }

    /// Source state for rendering input status; output acknowledgments are separate.
    pub fn controller(&self) -> &KeymapController {
        &self.controller
    }
    /// Whether an earlier submission failure has gated input.
    pub fn is_blocked(&self) -> bool {
        self.blocked
    }
    /// Adapter transport state, without asserting Android application receipt.
    pub fn adapter_state(&self) -> DeviceState {
        self.adapter.state()
    }
    /// Number of successfully submitted live contacts, unknown after transport error.
    pub fn submitted_active_count(&self) -> Option<usize> {
        self.adapter.active_count()
    }

    /// Translates and submits one owning-window event. Returns the number of
    /// successfully submitted transitions on success, not a downstream delivery ack.
    pub fn handle(&mut self, event: HostEvent) -> Result<usize, SessionError> {
        if self.blocked {
            return Err(SessionError::Blocked);
        }
        let transitions = self.controller.handle(event).map_err(SessionError::Input)?;
        self.submit(transitions)
    }

    /// Cancels and submits old contacts before applying a new content rectangle.
    pub fn set_viewport(&mut self, viewport: WindowViewport) -> Result<usize, SessionError> {
        if self.blocked {
            return Err(SessionError::Blocked);
        }
        let transitions = self.controller.set_viewport(viewport);
        self.submit(transitions)
    }

    /// Applies viewport/rotation changes after cleanup; rejects changed destination
    /// dimensions without mutation because device capabilities cannot change in place.
    pub fn set_geometry(
        &mut self,
        viewport: WindowViewport,
        size: AndroidSize,
        rotation: Rotation,
    ) -> Result<usize, SessionError> {
        if self.blocked {
            return Err(SessionError::Blocked);
        }
        if size != self.controller.android_size() {
            return Err(SessionError::DestinationChanged);
        }
        let transitions = self.controller.set_geometry(viewport, size, rotation);
        self.submit(transitions)
    }

    /// Explicitly clears all advertised adapter slots and source state. A failed
    /// cleanup stays blocked. Success still requires a new FocusGained event; held
    /// or queued source events must have been discarded by the host before this call.
    pub fn resynchronize(&mut self) -> Result<(), SessionError> {
        let _ = self.controller.handle(HostEvent::EmergencyRelease);
        self.blocked = true;
        self.adapter
            .resynchronize()
            .map_err(SessionError::Adapter)?;
        self.blocked = false;
        Ok(())
    }

    /// Gates the source, explicitly releases/reconciles the adapter, then destroys
    /// its transport on either result. An error means releases were not confirmed.
    pub fn close(mut self) -> Result<(), SessionError> {
        let _ = self.controller.handle(HostEvent::EmergencyRelease);
        self.adapter.close().map_err(SessionError::Adapter)
    }

    fn submit(&mut self, transitions: Vec<ContactTransition>) -> Result<usize, SessionError> {
        let count = transitions.len();
        for transition in transitions {
            if let Err(error) = self.adapter.submit(&[transition]) {
                self.blocked = true;
                let _ = self.controller.handle(HostEvent::EmergencyRelease);
                return Err(SessionError::Adapter(error));
            }
        }
        Ok(count)
    }
}
