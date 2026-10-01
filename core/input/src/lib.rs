//! Pure, synchronous multitouch state. This crate performs no device or network I/O.
//!
//! Coordinates are mapped from an edge-inclusive viewport into Android pixel indices.
//! Each successful call returns the transition(s) a future adapter must deliver in order.
//! Failed calls leave the allocator unchanged. Cleanup is explicit: callers must deliver
//! [`ContactAllocator::cancel_all`] or [`ContactAllocator::focus_lost`] transitions when
//! input is interrupted. Dropping the allocator cannot release contacts on a device.
//!
//! ```
//! use rex_input_core::{
//!     AndroidSize, BoundsPolicy, ContactAllocator, ContactId, CoordinateMapping,
//!     Rotation, Viewport,
//! };
//!
//! let mapping = CoordinateMapping::new(
//!     Viewport::new(20.0, 30.0, 800.0, 600.0)?,
//!     AndroidSize::new(1080, 1920)?,
//!     Rotation::None,
//!     BoundsPolicy::Reject,
//! );
//! let mut input = ContactAllocator::new(10, mapping)?;
//! let down = input.down(ContactId(7), 420.0, 330.0)?;
//! let moved = input.move_to(ContactId(7), 500.0, 400.0)?;
//! let released = input.focus_lost();
//! assert_eq!(released.len(), 1);
//! assert_eq!(input.active_count(), 0);
//! # let _ = (down, moved);
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::error::Error;
use std::fmt;

/// Library capacity limit. An adapter must also respect its actual device's limit.
pub const MAX_SLOTS: usize = 32;
/// Largest tracking identifier produced; all IDs fit a nonnegative signed 32-bit value.
pub const MAX_TRACKING_ID: u32 = i32::MAX as u32;

/// Invalid allocator or geometry configuration.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConfigError {
    /// The viewport has nonfinite values, nonpositive extents, or unrepresentable edges.
    InvalidViewport,
    /// Pixel dimensions must be between 1 and `i32::MAX`, inclusive.
    InvalidAndroidSize,
    /// The requested number of slots is outside `1..=MAX_SLOTS`.
    InvalidSlotCount,
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidViewport => "viewport must have finite, positive, representable extents",
            Self::InvalidAndroidSize => "Android dimensions must be in 1..=i32::MAX",
            Self::InvalidSlotCount => "slot count must be in 1..=MAX_SLOTS",
        })
    }
}

impl Error for ConfigError {}

/// An edge-inclusive rectangle in caller-supplied viewport units.
///
/// A caller must use the same units for this geometry and incoming points, excluding
/// letterboxing and converting logical/physical pixels before constructing it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Viewport {
    left: f64,
    top: f64,
    width: f64,
    height: f64,
    right: f64,
    bottom: f64,
}

impl Viewport {
    /// Validates a rectangle, including overflow and loss of extent at large origins.
    pub fn new(left: f64, top: f64, width: f64, height: f64) -> Result<Self, ConfigError> {
        let right = left + width;
        let bottom = top + height;
        // Use the representable edges as the effective extent. At a large origin,
        // adding a small extent may round; both accepted edges must still map exactly.
        let span_x = right - left;
        let span_y = bottom - top;
        if ![left, top, width, height, right, bottom, span_x, span_y]
            .iter()
            .all(|value| value.is_finite())
            || width <= 0.0
            || height <= 0.0
            || right <= left
            || bottom <= top
        {
            return Err(ConfigError::InvalidViewport);
        }
        Ok(Self {
            left,
            top,
            width: span_x,
            height: span_y,
            right,
            bottom,
        })
    }
}

/// Validated destination pixel dimensions; output indices are `0..width`, `0..height`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AndroidSize {
    width: u32,
    height: u32,
}

impl AndroidSize {
    /// Creates nonzero dimensions whose pixel indices fit a signed 32-bit adapter.
    pub fn new(width: u32, height: u32) -> Result<Self, ConfigError> {
        if width == 0 || height == 0 || width > i32::MAX as u32 || height > i32::MAX as u32 {
            return Err(ConfigError::InvalidAndroidSize);
        }
        Ok(Self { width, height })
    }

    /// Destination width in pixels.
    pub fn width(self) -> u32 {
        self.width
    }

    /// Destination height in pixels.
    pub fn height(self) -> u32 {
        self.height
    }
}

/// Clockwise transform of the normalized source point into destination coordinates.
///
/// These are explicit transforms, not Android display-orientation values. The caller
/// chooses the required direction and supplies the final destination dimensions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Rotation {
    /// `(u, v)`.
    None,
    /// `(1 - v, u)`.
    Clockwise90,
    /// `(1 - u, 1 - v)`.
    Clockwise180,
    /// `(v, 1 - u)`.
    Clockwise270,
}

/// Handling of finite points outside the viewport or normalized unit square.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BoundsPolicy {
    /// Return an error, without changing any active contact.
    Reject,
    /// Move the point to the nearest edge before rotation.
    Clamp,
}

/// Integer destination pixel indices, always bounded when produced by a mapping.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PixelPoint {
    /// Horizontal index.
    pub x: u32,
    /// Vertical index.
    pub y: u32,
}

/// A point cannot be mapped under the configured policy.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MappingError {
    /// NaN and infinities are rejected even under the clamp policy.
    NonFinitePoint,
    /// A viewport point is outside the edge-inclusive viewport.
    OutsideViewport,
    /// A normalized point is outside the edge-inclusive `[0, 1]` unit square.
    OutsideNormalizedBounds,
}

impl fmt::Display for MappingError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::NonFinitePoint => "point coordinates must be finite",
            Self::OutsideViewport => "point is outside the viewport",
            Self::OutsideNormalizedBounds => "normalized coordinates must be in [0, 1]",
        })
    }
}

impl Error for MappingError {}

/// Immutable, validated viewport-to-pixel mapping.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CoordinateMapping {
    viewport: Viewport,
    android: AndroidSize,
    rotation: Rotation,
    bounds: BoundsPolicy,
}

impl CoordinateMapping {
    /// Combines validated geometry with an explicit transform and boundary policy.
    pub fn new(
        viewport: Viewport,
        android: AndroidSize,
        rotation: Rotation,
        bounds: BoundsPolicy,
    ) -> Self {
        Self {
            viewport,
            android,
            rotation,
            bounds,
        }
    }

    /// Maps viewport coordinates, with inclusive edges and nearest-pixel rounding.
    /// Midpoint ties round up. Out-of-range validation precedes subtraction to avoid
    /// overflow with very large but finite inputs.
    pub fn map_viewport(self, x: f64, y: f64) -> Result<PixelPoint, MappingError> {
        finite_point(x, y)?;
        let v = self.viewport;
        if self.bounds == BoundsPolicy::Reject
            && (x < v.left || x > v.right || y < v.top || y > v.bottom)
        {
            return Err(MappingError::OutsideViewport);
        }
        let u = ((x.clamp(v.left, v.right) - v.left) / v.width).clamp(0.0, 1.0);
        let v = ((y.clamp(v.top, v.bottom) - v.top) / v.height).clamp(0.0, 1.0);
        Ok(self.map_unit_square(u, v))
    }

    /// Maps a normalized point in `[0, 1]` using the same bounds policy and rotation.
    pub fn map_normalized(self, u: f64, v: f64) -> Result<PixelPoint, MappingError> {
        finite_point(u, v)?;
        if self.bounds == BoundsPolicy::Reject
            && (!(0.0..=1.0).contains(&u) || !(0.0..=1.0).contains(&v))
        {
            return Err(MappingError::OutsideNormalizedBounds);
        }
        Ok(self.map_unit_square(u.clamp(0.0, 1.0), v.clamp(0.0, 1.0)))
    }

    fn map_unit_square(self, u: f64, v: f64) -> PixelPoint {
        let (u, v) = match self.rotation {
            Rotation::None => (u, v),
            Rotation::Clockwise90 => (1.0 - v, u),
            Rotation::Clockwise180 => (1.0 - u, 1.0 - v),
            Rotation::Clockwise270 => (v, 1.0 - u),
        };
        PixelPoint {
            x: (u * f64::from(self.android.width - 1)).round() as u32,
            y: (v * f64::from(self.android.height - 1)).round() as u32,
        }
    }
}

fn finite_point(x: f64, y: f64) -> Result<(), MappingError> {
    if x.is_finite() && y.is_finite() {
        Ok(())
    } else {
        Err(MappingError::NonFinitePoint)
    }
}

/// Caller-owned input identity, unique among currently active contacts.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ContactId(pub u64);

/// A contact snapshot. Identity, slot, and tracking ID are stable until release.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Contact {
    /// The caller's identity.
    pub id: ContactId,
    /// Zero-based slot, strictly below the allocator's configured capacity.
    pub slot: usize,
    /// Monotonic ID, never reused during this allocator's lifetime.
    pub tracking_id: u32,
    /// Last accepted mapped position.
    pub position: PixelPoint,
}

/// Why an active contact was released.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReleaseReason {
    /// Normal end of an individual contact.
    Up,
    /// Input stream was explicitly canceled or interrupted.
    Cancelled,
    /// Host application lost focus.
    FocusLost,
    /// Viewport, orientation, or destination geometry changed.
    MappingChanged,
}

/// Adapter-neutral transition. Deliver all returned transitions in call order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "transitions must be delivered by the caller's adapter"]
pub enum ContactTransition {
    /// Begin a new contact.
    Down(Contact),
    /// Update an existing contact's position.
    Move(Contact),
    /// End an existing contact at its last accepted position.
    Up {
        /// Contact being released.
        contact: Contact,
        /// Reason for cleanup.
        reason: ReleaseReason,
    },
}

/// A requested transition was rejected; state and tracking-ID sequence are unchanged.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputError {
    /// A second down for a still-active identity is an explicit error.
    AlreadyActive(ContactId),
    /// A move requires a currently active identity.
    UnknownContact(ContactId),
    /// All configured slots are in use. Existing contacts are never evicted.
    CapacityExceeded,
    /// This allocator has consumed its complete tracking-ID range.
    TrackingIdsExhausted,
    /// The point could not be mapped under the configured policy.
    Mapping(MappingError),
}

impl fmt::Display for InputError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::AlreadyActive(id) => write!(f, "contact {} is already active", id.0),
            Self::UnknownContact(id) => write!(f, "contact {} is not active", id.0),
            Self::CapacityExceeded => f.write_str("all contact slots are in use"),
            Self::TrackingIdsExhausted => f.write_str("tracking identifiers are exhausted"),
            Self::Mapping(error) => error.fmt(f),
        }
    }
}

impl Error for InputError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Mapping(error) => Some(error),
            _ => None,
        }
    }
}

impl From<MappingError> for InputError {
    fn from(error: MappingError) -> Self {
        Self::Mapping(error)
    }
}

/// Single-stream allocator. All contact state is local to this object.
///
/// This object deliberately has no `Clone` or I/O-owning destructor. The owner must
/// serialize events, stop accepting source input after focus loss, and send cleanup
/// before dropping or replacing a live adapter. A rejected move leaves the contact
/// active so a later `up`, cancellation, or focus loss can still release it.
#[derive(Debug)]
pub struct ContactAllocator {
    mapping: CoordinateMapping,
    slots: Vec<Option<Contact>>,
    next_tracking_id: u64,
}

impl ContactAllocator {
    /// Creates an empty allocator. Tracking IDs start at zero.
    pub fn new(max_slots: usize, mapping: CoordinateMapping) -> Result<Self, ConfigError> {
        if !(1..=MAX_SLOTS).contains(&max_slots) {
            return Err(ConfigError::InvalidSlotCount);
        }
        Ok(Self {
            mapping,
            slots: vec![None; max_slots],
            next_tracking_id: 0,
        })
    }

    /// Maximum number of simultaneous contacts.
    pub fn capacity(&self) -> usize {
        self.slots.len()
    }

    /// Number of currently active contacts.
    pub fn active_count(&self) -> usize {
        self.contacts().count()
    }

    /// Active contact snapshots in ascending slot order.
    pub fn contacts(&self) -> impl Iterator<Item = Contact> + '_ {
        self.slots.iter().filter_map(|contact| *contact)
    }

    /// Looks up the current snapshot for a caller identity.
    pub fn contact(&self, id: ContactId) -> Option<Contact> {
        self.contacts().find(|contact| contact.id == id)
    }

    /// Begins a contact in the lowest free slot. All validation precedes mutation.
    /// Repeated downs are errors, including when the repeated point is invalid.
    pub fn down(&mut self, id: ContactId, x: f64, y: f64) -> Result<ContactTransition, InputError> {
        if self.contact(id).is_some() {
            return Err(InputError::AlreadyActive(id));
        }
        let position = self.mapping.map_viewport(x, y)?;
        let slot = self
            .slots
            .iter()
            .position(Option::is_none)
            .ok_or(InputError::CapacityExceeded)?;
        if self.next_tracking_id > u64::from(MAX_TRACKING_ID) {
            return Err(InputError::TrackingIdsExhausted);
        }
        let contact = Contact {
            id,
            slot,
            tracking_id: self.next_tracking_id as u32,
            position,
        };
        self.slots[slot] = Some(contact);
        self.next_tracking_id += 1;
        Ok(ContactTransition::Down(contact))
    }

    /// Moves an active contact; returns `None` when the mapped pixel is unchanged.
    pub fn move_to(
        &mut self,
        id: ContactId,
        x: f64,
        y: f64,
    ) -> Result<Option<ContactTransition>, InputError> {
        let mut contact = self.contact(id).ok_or(InputError::UnknownContact(id))?;
        let position = self.mapping.map_viewport(x, y)?;
        if contact.position == position {
            return Ok(None);
        }
        contact.position = position;
        self.slots[contact.slot] = Some(contact);
        Ok(Some(ContactTransition::Move(contact)))
    }

    /// Releases a contact without requiring valid coordinates. Unknown or already
    /// released identities return `None`, making duplicate/interrupted ups harmless.
    #[must_use = "release transitions must be delivered to the adapter"]
    pub fn up(&mut self, id: ContactId) -> Option<ContactTransition> {
        let contact = self.contact(id)?;
        self.slots[contact.slot] = None;
        Some(ContactTransition::Up {
            contact,
            reason: ReleaseReason::Up,
        })
    }

    /// Releases every contact in slot order. Repeated cancellation returns no events.
    #[must_use = "cleanup transitions must be delivered to the adapter"]
    pub fn cancel_all(&mut self) -> Vec<ContactTransition> {
        self.release_all(ReleaseReason::Cancelled)
    }

    /// Releases every contact in slot order. The owner must gate future input until
    /// focus is restored; this method does not observe or track OS focus.
    #[must_use = "cleanup transitions must be delivered to the adapter"]
    pub fn focus_lost(&mut self) -> Vec<ContactTransition> {
        self.release_all(ReleaseReason::FocusLost)
    }

    /// Releases active contacts using their old coordinates before replacing mapping.
    /// Deliver these releases before any new down, even if the mapping is unchanged.
    #[must_use = "cleanup transitions must be delivered before using the new mapping"]
    pub fn set_mapping(&mut self, mapping: CoordinateMapping) -> Vec<ContactTransition> {
        let releases = self.release_all(ReleaseReason::MappingChanged);
        self.mapping = mapping;
        releases
    }

    fn release_all(&mut self, reason: ReleaseReason) -> Vec<ContactTransition> {
        let mut releases = Vec::with_capacity(self.active_count());
        for slot in &mut self.slots {
            if let Some(contact) = slot.take() {
                releases.push(ContactTransition::Up { contact, reason });
            }
        }
        releases
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tracking_exhaustion_is_fail_closed_and_cleanup_still_works() {
        let mapping = CoordinateMapping::new(
            Viewport::new(0.0, 0.0, 1.0, 1.0).unwrap(),
            AndroidSize::new(2, 2).unwrap(),
            Rotation::None,
            BoundsPolicy::Reject,
        );
        let mut input = ContactAllocator::new(2, mapping).unwrap();
        input.next_tracking_id = u64::from(MAX_TRACKING_ID);
        let last = input.down(ContactId(1), 0.0, 0.0).unwrap();
        assert!(matches!(last, ContactTransition::Down(c) if c.tracking_id == MAX_TRACKING_ID));
        assert_eq!(
            input.down(ContactId(2), 0.0, 0.0),
            Err(InputError::TrackingIdsExhausted)
        );
        assert_eq!(input.active_count(), 1);
        assert!(matches!(
            input.move_to(ContactId(1), 1.0, 1.0),
            Ok(Some(ContactTransition::Move(c))) if c.tracking_id == MAX_TRACKING_ID
        ));
        assert!(input.up(ContactId(1)).is_some());
        assert_eq!(
            input.down(ContactId(2), 0.0, 0.0),
            Err(InputError::TrackingIdsExhausted)
        );
        assert!(input.cancel_all().is_empty());
    }
}
