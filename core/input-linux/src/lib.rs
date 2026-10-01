//! Linux type-B multitouch output for [`rex_input_core`] transitions.
//!
//! [`LinuxTouchDevice::create`] explicitly creates one virtual touchscreen. There
//! is no input capture, global hook, privilege setup, reconnect, or background I/O.
//! A successful frame means the transport reports body and `SYN_REPORT` completion,
//! under its documented completion contract, **not** downstream receipt. Every transport error
//! latches [`DeviceState::Faulted`]; further input is rejected until an explicit
//! [`TouchAdapter::resynchronize`] successfully submits releases for every slot.
//!
//! The allocator advances before transport I/O. After an I/O error, stop source
//! input, discard queued transitions, clear the allocator with `cancel_all`, and
//! resynchronize the adapter (or destroy both objects). Do not retry the failed
//! batch. Keep the existing allocator after resynchronization: tracking IDs must
//! continue increasing. Destination dimension/slot changes require a new device.
//!
//! Call [`TouchAdapter::close`] for observable cleanup at shutdown. Dropping a
//! live device closes its uinput handle, but does not report successful release
//! delivery. No code here opens another device or tests live input automatically.
//!
//! ```no_run
//! use rex_input_core::{
//!     AndroidSize, BoundsPolicy, ContactAllocator, ContactId, CoordinateMapping,
//!     Rotation, Viewport,
//! };
//! use rex_input_linux::{DeviceConfig, LinuxTouchDevice};
//!
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let size = AndroidSize::new(1080, 1920)?;
//! let mapping = CoordinateMapping::new(
//!     Viewport::new(0.0, 0.0, 1080.0, 1920.0)?, size,
//!     Rotation::None, BoundsPolicy::Reject,
//! );
//! let mut source = ContactAllocator::new(10, mapping)?;
//! // This is the only call that opens /dev/uinput. Coordinate consumer readiness
//! // externally before sending any input to this intentionally virtual device.
//! let mut device = LinuxTouchDevice::create(DeviceConfig::new(size, 10)?)?;
//! device.submit(&[source.down(ContactId(1), 100.0, 200.0)?])?;
//! // Gate new source input before focus-loss cleanup.
//! device.submit(&source.focus_lost())?;
//! device.close()?;
//! // On any error above, this function exits and destroys the owned device.
//! # Ok(())
//! # }
//! ```
//!
//! This crate exposes its adapter API only on Linux. Other platforms should use
//! a target-specific dependency and their own transport implementation.

#![forbid(unsafe_code)]
#![warn(missing_docs)]
#![cfg(target_os = "linux")]

use evdev::{
    uinput::VirtualDevice, AbsInfo, AbsoluteAxisCode, AttributeSet, BusType, EventType, InputEvent,
    InputId, KeyCode, PropType, UinputAbsSetup,
};
use rex_input_core::{AndroidSize, Contact, ContactTransition, MAX_SLOTS, MAX_TRACKING_ID};
use std::{error::Error, fmt, io, path::PathBuf};

/// The advertised name is deliberately recognizable as a virtual touchscreen.
pub const DEVICE_NAME: &str = "RexPlayer Virtual Touchscreen";
/// Linux's MT slot initialization fixes the advertised wire tracking range to 16 bits.
/// Core IDs remain separate monotonic identities; live wire IDs are never duplicated.
pub const MAX_KERNEL_TRACKING_ID: u16 = u16::MAX;

/// Validated capabilities, fixed for the lifetime of the virtual device.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DeviceConfig {
    size: AndroidSize,
    slots: usize,
}

impl DeviceConfig {
    /// Sets pixel dimensions and `1..=rex_input_core::MAX_SLOTS` contact slots.
    /// Configure the source allocator with the same dimensions and slot count.
    pub fn new(size: AndroidSize, slots: usize) -> Result<Self, InvalidTransition> {
        if !(1..=MAX_SLOTS).contains(&slots) {
            return Err(InvalidTransition::InvalidSlotCount);
        }
        Ok(Self { size, slots })
    }

    /// Destination dimensions, with inclusive axis maxima of width/height minus one.
    pub fn size(self) -> AndroidSize {
        self.size
    }

    /// Number of independently tracked type-B contact slots.
    pub fn slots(self) -> usize {
        self.slots
    }
}

/// Rejection before any transport I/O. The adapter's state remains unchanged.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InvalidTransition {
    /// A configuration requested zero or more than the core's maximum slots.
    InvalidSlotCount,
    /// A frame contains more transitions than configured slots.
    FrameTooLarge,
    /// A contact slot lies outside the configured device capabilities.
    SlotOutOfRange,
    /// A frame changes the same slot more than once; split its lifecycle into frames.
    RepeatedSlot,
    /// Coordinates lie outside the configured device capabilities.
    PositionOutOfRange,
    /// Tracking IDs must fit a nonnegative signed 32-bit integer.
    TrackingIdOutOfRange,
    /// A new contact's tracking ID does not increase beyond every attempted down.
    TrackingIdNotIncreasing,
    /// A down refers to an occupied slot.
    SlotOccupied,
    /// An identity is already active in another slot, even if released in this frame.
    DuplicateIdentity,
    /// A move or up does not match the slot's active identity and tracking ID.
    UnknownContact,
    /// An up's last position differs from the last successfully submitted position.
    StaleRelease,
}

impl fmt::Display for InvalidTransition {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidSlotCount => "slot count must be in 1..=rex_input_core::MAX_SLOTS",
            Self::FrameTooLarge => "frame has more transitions than device slots",
            Self::SlotOutOfRange => "contact slot is outside device capabilities",
            Self::RepeatedSlot => "a frame may change each slot only once",
            Self::PositionOutOfRange => "contact position is outside device capabilities",
            Self::TrackingIdOutOfRange => "tracking ID exceeds i32::MAX",
            Self::TrackingIdNotIncreasing => "down tracking IDs must strictly increase",
            Self::SlotOccupied => "down requires an empty slot",
            Self::DuplicateIdentity => "contact identity is already active",
            Self::UnknownContact => "transition does not match an active contact",
            Self::StaleRelease => "release position differs from the submitted contact",
        })
    }
}

impl Error for InvalidTransition {}

/// Errors distinguish rejected input from uncertain device state after I/O.
#[derive(Debug)]
pub enum AdapterError {
    /// Input validation failed. Nothing was written and local state is unchanged.
    Invalid(InvalidTransition),
    /// A frame may have been partially written, including a failed `SYN_REPORT`.
    Transport(io::Error),
    /// Input is disabled after a transport error. Resynchronize or destroy the device.
    Faulted,
}

impl fmt::Display for AdapterError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Invalid(error) => error.fmt(f),
            Self::Transport(error) => {
                write!(f, "input transport failed; device state unknown: {error}")
            }
            Self::Faulted => f.write_str("input device is faulted; resynchronize or destroy it"),
        }
    }
}

impl Error for AdapterError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Invalid(error) => Some(error),
            Self::Transport(error) => Some(error),
            Self::Faulted => None,
        }
    }
}

impl From<InvalidTransition> for AdapterError {
    fn from(error: InvalidTransition) -> Self {
        Self::Invalid(error)
    }
}

/// Whether the adapter's submitted contact state is known.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceState {
    /// Transport reported completion under its contract; not downstream acknowledgment.
    Ready,
    /// A write failed. The kernel may have received any prefix of the failed frame.
    Faulted,
}

/// A synchronous, exclusive writer for one initially empty type-B touch device.
///
/// Implementations must write all supplied events **in order**, then exactly one
/// `EV_SYN/SYN_REPORT`. Return success only after the underlying completion API
/// reports success, and propagate every partial-write or synchronization error.
/// Byte-oriented implementations must reject zero progress with data remaining;
/// wrappers of event APIs must document their completion assumptions, as does
/// [`UinputTransport`]. Input slices contain no synchronization events. Do not
/// silently reconnect or
/// share the device with another writer. Implementations must advertise the same
/// capabilities as the adapter's [`DeviceConfig`].
///
/// This seam supports deterministic tests without opening `/dev/uinput`.
pub trait FrameTransport {
    /// Submit one whole frame, appending exactly one `SYN_REPORT`.
    fn emit_frame(&mut self, events: &[InputEvent]) -> io::Result<()>;
}

/// Owns only the virtual device created by [`LinuxTouchDevice::create`].
///
/// Uses evdev 0.13.2's writer and propagates its errors. Its byte-write helper
/// treats a zero-byte write as termination; correctness therefore relies on the
/// Linux uinput driver's complete-event write semantics. This crate does not
/// independently observe raw syscall byte counts or inject failures into evdev.
#[derive(Debug)]
pub struct UinputTransport {
    device: VirtualDevice,
}

impl FrameTransport for UinputTransport {
    fn emit_frame(&mut self, events: &[InputEvent]) -> io::Result<()> {
        // evdev 0.13.2 completes the body write, then writes exactly one SYN_REPORT.
        // Errors from either phase propagate; the adapter cannot assume atomicity.
        self.device.emit(events)
    }
}

/// Validates and translates ordered core transitions into bounded Linux frames.
///
/// Access requires `&mut self`; there is no shared writer or hidden worker thread.
/// At most one transition per slot is allowed in a frame, so a down and its up
/// cannot disappear inside one kernel synchronization interval. Supply downs in
/// allocator order, with strictly increasing tracking IDs. Moves/releases preserve
/// contact identity. `ABS_X/Y` follows the oldest remaining contact for legacy
/// single-touch consumers. Core tracking IDs are mapped to distinct active 16-bit
/// kernel IDs because Linux's MT initialization fixes that advertised wire range.
/// Wire IDs wrap while skipping every active ID, including pre-frame releases.
#[derive(Debug)]
pub struct TouchAdapter<T: FrameTransport> {
    transport: T,
    config: DeviceConfig,
    slots: Vec<Option<Contact>>,
    kernel_tracking_ids: Vec<Option<u16>>,
    next_kernel_tracking_id: u16,
    last_tracking_id: Option<u32>,
    state: DeviceState,
}

/// The real Linux uinput adapter. Construction is explicit and may need existing
/// device permissions; this crate never creates permissions or escalates privileges.
pub type LinuxTouchDevice = TouchAdapter<UinputTransport>;

impl LinuxTouchDevice {
    /// Opens `/dev/uinput` and creates one `INPUT_PROP_DIRECT` virtual touchscreen.
    ///
    /// Advertises only `BTN_TOUCH`, `ABS_X/Y`, the MT position/tracking axes, and
    /// `ABS_MT_SLOT`. No physical device is opened or grabbed. Creation errors are
    /// returned directly. Consumers may need time to discover the new device;
    /// coordinate their readiness before sending input.
    pub fn create(config: DeviceConfig) -> io::Result<Self> {
        let mut keys = AttributeSet::<KeyCode>::new();
        keys.insert(KeyCode::BTN_TOUCH);
        let mut properties = AttributeSet::<PropType>::new();
        properties.insert(PropType::DIRECT);
        let mut builder = VirtualDevice::builder()?
            .name(DEVICE_NAME)
            .input_id(InputId::new(BusType::BUS_VIRTUAL, 0, 0, 1))
            .with_keys(&keys)?
            .with_properties(&properties)?;
        for axis in absolute_axes(config) {
            builder = builder.with_absolute_axis(&axis)?;
        }
        let transport = UinputTransport {
            device: builder.build()?,
        };
        Ok(Self::with_transport(config, transport))
    }

    /// Lists event-node paths belonging to this device only, without opening them.
    /// The list may be empty before device discovery has finished.
    pub fn device_nodes(&mut self) -> io::Result<Vec<PathBuf>> {
        self.transport
            .device
            .enumerate_dev_nodes_blocking()?
            .collect()
    }
}

impl<T: FrameTransport> TouchAdapter<T> {
    /// Wraps an exclusive, initially empty transport with matching capabilities.
    /// This does not open a device, emit events, or verify an arbitrary transport.
    pub fn with_transport(config: DeviceConfig, transport: T) -> Self {
        Self {
            transport,
            config,
            slots: vec![None; config.slots],
            kernel_tracking_ids: vec![None; config.slots],
            next_kernel_tracking_id: 0,
            last_tracking_id: None,
            state: DeviceState::Ready,
        }
    }

    /// Returns this device's immutable capabilities.
    pub fn config(&self) -> DeviceConfig {
        self.config
    }

    /// Returns whether ordinary input can be submitted.
    pub fn state(&self) -> DeviceState {
        self.state
    }

    /// Successfully submitted active contacts, or `None` after any transport error.
    /// This count never implies acknowledgment from a downstream input consumer.
    pub fn active_count(&self) -> Option<usize> {
        (self.state == DeviceState::Ready).then(|| self.slots.iter().flatten().count())
    }

    /// Validates the entire batch before writing one synchronization frame.
    ///
    /// Empty input performs no I/O. Validation errors are side-effect-free. On a
    /// transport error, no rollback is claimed and all further batches (even empty
    /// ones) fail with [`AdapterError::Faulted`]. Stop the source stream immediately.
    pub fn submit(&mut self, transitions: &[ContactTransition]) -> Result<(), AdapterError> {
        if self.state == DeviceState::Faulted {
            return Err(AdapterError::Faulted);
        }
        if transitions.is_empty() {
            return Ok(());
        }
        if transitions.len() > self.config.slots {
            return Err(InvalidTransition::FrameTooLarge.into());
        }
        let mut next = self.slots.clone();
        let mut kernel_ids = self.kernel_tracking_ids.clone();
        let mut next_kernel_id = self.next_kernel_tracking_id;
        let mut seen = vec![false; self.config.slots];
        let mut next_tracking_id = self.last_tracking_id;
        for transition in transitions {
            let contact = match transition {
                ContactTransition::Down(c) | ContactTransition::Move(c) => c,
                ContactTransition::Up { contact, .. } => contact,
            };
            self.validate_contact(*contact)?;
            if seen[contact.slot] {
                return Err(InvalidTransition::RepeatedSlot.into());
            }
            seen[contact.slot] = true;
            match transition {
                ContactTransition::Down(_) => {
                    if self.slots[contact.slot].is_some() {
                        return Err(InvalidTransition::SlotOccupied.into());
                    }
                    if self.slots.iter().flatten().any(|c| c.id == contact.id)
                        || next.iter().flatten().any(|c| c.id == contact.id)
                    {
                        return Err(InvalidTransition::DuplicateIdentity.into());
                    }
                    if next_tracking_id.is_some_and(|last| contact.tracking_id <= last) {
                        return Err(InvalidTransition::TrackingIdNotIncreasing.into());
                    }
                    next_tracking_id = Some(contact.tracking_id);
                    kernel_ids[contact.slot] = Some(allocate_kernel_id(
                        &mut next_kernel_id,
                        &self.kernel_tracking_ids,
                        &kernel_ids,
                    ));
                    next[contact.slot] = Some(*contact);
                }
                ContactTransition::Move(_) | ContactTransition::Up { .. } => {
                    let old = self.slots[contact.slot]
                        .filter(|c| c.id == contact.id && c.tracking_id == contact.tracking_id)
                        .ok_or(InvalidTransition::UnknownContact)?;
                    if matches!(transition, ContactTransition::Up { .. }) {
                        if old.position != contact.position {
                            return Err(InvalidTransition::StaleRelease.into());
                        }
                        next[contact.slot] = None;
                        kernel_ids[contact.slot] = None;
                    } else {
                        next[contact.slot] = Some(*contact);
                    }
                }
            }
        }
        let primary = next.iter().flatten().min_by_key(|c| c.tracking_id);
        // A bounded frame: BTN_TOUCH + at most four events per slot + legacy X/Y.
        let mut events = Vec::with_capacity(1 + 4 * transitions.len() + 2);
        events.push(touch(primary.is_some()));
        for transition in transitions {
            match transition {
                ContactTransition::Down(contact) => {
                    events.push(abs(AbsoluteAxisCode::ABS_MT_SLOT, contact.slot as i32));
                    events.push(abs(
                        AbsoluteAxisCode::ABS_MT_TRACKING_ID,
                        i32::from(kernel_ids[contact.slot].expect("validated down has a wire ID")),
                    ));
                    position_events(&mut events, *contact);
                }
                ContactTransition::Move(contact) => {
                    events.push(abs(AbsoluteAxisCode::ABS_MT_SLOT, contact.slot as i32));
                    position_events(&mut events, *contact);
                }
                ContactTransition::Up { contact, .. } => {
                    events.push(abs(AbsoluteAxisCode::ABS_MT_SLOT, contact.slot as i32));
                    events.push(abs(AbsoluteAxisCode::ABS_MT_TRACKING_ID, -1));
                }
            }
        }
        if let Some(primary) = primary {
            events.push(abs(AbsoluteAxisCode::ABS_X, primary.position.x as i32));
            events.push(abs(AbsoluteAxisCode::ABS_Y, primary.position.y as i32));
        }
        // Attempted IDs remain consumed even if the kernel accepts only a prefix.
        self.last_tracking_id = next_tracking_id;
        self.next_kernel_tracking_id = next_kernel_id;
        self.emit(&events)?;
        self.slots = next;
        self.kernel_tracking_ids = kernel_ids;
        Ok(())
    }

    /// Releases every known active slot in one frame. With no contacts, does no I/O.
    ///
    /// Useful for focus loss or shutdown after gating source input. Also clear the
    /// source allocator and discard its returned releases instead of submitting
    /// them twice. A faulted device requires [`Self::resynchronize`] instead.
    pub fn release_all(&mut self) -> Result<(), AdapterError> {
        if self.state == DeviceState::Faulted {
            return Err(AdapterError::Faulted);
        }
        if self.slots.iter().all(Option::is_none) {
            return Ok(());
        }
        let mut events = vec![touch(false)];
        for (slot, contact) in self.slots.iter().enumerate() {
            if contact.is_some() {
                events.push(abs(AbsoluteAxisCode::ABS_MT_SLOT, slot as i32));
                events.push(abs(AbsoluteAxisCode::ABS_MT_TRACKING_ID, -1));
            }
        }
        self.emit(&events)?;
        self.slots.fill(None);
        self.kernel_tracking_ids.fill(None);
        Ok(())
    }

    /// Explicitly resets every advertised slot, including slots possibly written
    /// by a failed frame. Input is enabled only after the entire cleanup succeeds.
    ///
    /// First stop source input, discard queued transitions, and clear its allocator.
    /// Keep the allocator's increasing tracking-ID sequence. This operation does
    /// not reconnect, recreate the device, or prove downstream delivery. A failed
    /// cleanup keeps the adapter faulted and its active count unknown.
    pub fn resynchronize(&mut self) -> Result<(), AdapterError> {
        let mut events = Vec::with_capacity(1 + 2 * self.config.slots);
        events.push(touch(false));
        for slot in 0..self.config.slots {
            events.push(abs(AbsoluteAxisCode::ABS_MT_SLOT, slot as i32));
            events.push(abs(AbsoluteAxisCode::ABS_MT_TRACKING_ID, -1));
        }
        self.emit(&events)?;
        self.slots.fill(None);
        self.kernel_tracking_ids.fill(None);
        self.state = DeviceState::Ready;
        Ok(())
    }

    /// Consumes the adapter after explicit cleanup and drops its transport on both
    /// success and failure. A faulted device attempts a full resynchronization.
    /// Returned errors must not be interpreted as successfully delivered releases.
    pub fn close(mut self) -> Result<(), AdapterError> {
        if self.state == DeviceState::Faulted {
            self.resynchronize()
        } else {
            self.release_all()
        }
    }

    fn validate_contact(&self, contact: Contact) -> Result<(), InvalidTransition> {
        if contact.slot >= self.config.slots {
            return Err(InvalidTransition::SlotOutOfRange);
        }
        if contact.position.x >= self.config.size.width()
            || contact.position.y >= self.config.size.height()
        {
            return Err(InvalidTransition::PositionOutOfRange);
        }
        if contact.tracking_id > MAX_TRACKING_ID {
            return Err(InvalidTransition::TrackingIdOutOfRange);
        }
        Ok(())
    }

    fn emit(&mut self, events: &[InputEvent]) -> Result<(), AdapterError> {
        self.transport.emit_frame(events).map_err(|error| {
            self.state = DeviceState::Faulted;
            AdapterError::Transport(error)
        })
    }
}

fn allocate_kernel_id(next: &mut u16, before: &[Option<u16>], after: &[Option<u16>]) -> u16 {
    // Downs use originally empty slots, so this union has at most MAX_SLOTS IDs.
    // Keeping pre-frame IDs reserved avoids release/reuse in the same frame.
    while before.contains(&Some(*next)) || after.contains(&Some(*next)) {
        *next = next.wrapping_add(1);
    }
    let result = *next;
    *next = next.wrapping_add(1);
    result
}

fn absolute_axes(config: DeviceConfig) -> [UinputAbsSetup; 6] {
    let x_max = (config.size.width() - 1) as i32;
    let y_max = (config.size.height() - 1) as i32;
    [
        (AbsoluteAxisCode::ABS_MT_SLOT, (config.slots - 1) as i32),
        (
            AbsoluteAxisCode::ABS_MT_TRACKING_ID,
            i32::from(MAX_KERNEL_TRACKING_ID),
        ),
        (AbsoluteAxisCode::ABS_MT_POSITION_X, x_max),
        (AbsoluteAxisCode::ABS_MT_POSITION_Y, y_max),
        (AbsoluteAxisCode::ABS_X, x_max),
        (AbsoluteAxisCode::ABS_Y, y_max),
    ]
    .map(|(code, max)| UinputAbsSetup::new(code, AbsInfo::new(0, 0, max, 0, 0, 0)))
}

fn touch(active: bool) -> InputEvent {
    InputEvent::new(EventType::KEY.0, KeyCode::BTN_TOUCH.0, i32::from(active))
}

fn abs(code: AbsoluteAxisCode, value: i32) -> InputEvent {
    InputEvent::new(EventType::ABSOLUTE.0, code.0, value)
}

fn position_events(events: &mut Vec<InputEvent>, contact: Contact) {
    events.push(abs(
        AbsoluteAxisCode::ABS_MT_POSITION_X,
        contact.position.x as i32,
    ));
    events.push(abs(
        AbsoluteAxisCode::ABS_MT_POSITION_Y,
        contact.position.y as i32,
    ));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kernel_ids_wrap_without_colliding_with_live_or_same_frame_released_ids() {
        let mut next = u16::MAX;
        let before = [Some(0), Some(1), None, None];
        let after = [Some(0), None, Some(2), None];
        assert_eq!(allocate_kernel_id(&mut next, &before, &after), u16::MAX);
        assert_eq!(allocate_kernel_id(&mut next, &before, &after), 3);
        assert_eq!(next, 4);
    }

    #[test]
    fn capabilities_have_exact_bounded_ranges_and_no_invented_physical_resolution() {
        let config = DeviceConfig::new(AndroidSize::new(1080, 1920).unwrap(), 10).unwrap();
        let axes = absolute_axes(config);
        let expected = [
            (AbsoluteAxisCode::ABS_MT_SLOT, 9),
            (
                AbsoluteAxisCode::ABS_MT_TRACKING_ID,
                i32::from(MAX_KERNEL_TRACKING_ID),
            ),
            (AbsoluteAxisCode::ABS_MT_POSITION_X, 1079),
            (AbsoluteAxisCode::ABS_MT_POSITION_Y, 1919),
            (AbsoluteAxisCode::ABS_X, 1079),
            (AbsoluteAxisCode::ABS_Y, 1919),
        ];
        for (axis, (code, maximum)) in axes.iter().zip(expected) {
            assert_eq!(axis.code(), code.0);
            assert_eq!(axis.absinfo().minimum(), 0);
            assert_eq!(axis.absinfo().maximum(), maximum);
            assert_eq!(axis.absinfo().resolution(), 0);
            assert_eq!(axis.absinfo().fuzz(), 0);
            assert_eq!(axis.absinfo().flat(), 0);
        }
    }
}
