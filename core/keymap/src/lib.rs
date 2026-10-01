//! Validated, bounded, window-scoped host input translation.
//!
//! This crate captures no input and opens no devices. Feed only the current player
//! window's events, in order. The controller starts unfocused. Each returned
//! transition must be submitted in its own adapter frame, in order: a tap and a
//! joystick start can change the same slot twice. Never retry a partly sent batch.
//! On transport failure, gate input, discard the batch, clear the controller with
//! [`HostEvent::EmergencyRelease`], and reconcile the adapter before resuming.
//!
//! Coordinates in a profile are normalized within the displayed content before
//! the explicit rotation. Host mouse coordinates use the viewport's units. Supply
//! the actual content rectangle, excluding letterboxing, in the same DPI units as
//! the host events. No timers, key synthesis, global capture, or pointer grab is used.

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use rex_input_core::{
    AndroidSize, BoundsPolicy, ConfigError, ContactAllocator, ContactId, ContactTransition,
    CoordinateMapping, InputError, Rotation, Viewport, MAX_SLOTS,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashSet,
    error::Error,
    fmt,
    io::{self, Read},
    str::FromStr,
};

#[cfg(all(target_os = "linux", feature = "linux-adapter"))]
pub mod linux;

#[cfg(all(target_os = "linux", feature = "linux-adapter"))]
pub mod worker;

/// Largest accepted UTF-8 JSON profile, checked before parsing.
pub const MAX_PROFILE_BYTES: usize = 64 * 1024;
/// Maximum independent contact owners, including a enabled direct mouse pointer.
pub const MAX_BINDINGS: usize = MAX_SLOTS;
/// Current profile schema version. Unsupported versions are rejected.
pub const PROFILE_VERSION: u32 = 1;
/// Small, usable example; importing it does not enable input.
pub const DEFAULT_KEYMAP_JSON: &str = include_str!("../examples/default.json");

/// Invalid profile, geometry, host key, or input transition.
#[derive(Debug)]
pub enum KeymapError {
    /// Profile exceeded its pre-parse byte limit.
    ProfileTooLarge,
    /// Reading a profile failed; no partial profile is returned.
    Read(io::Error),
    /// Invalid JSON, unexpected fields, duplicate fields, or incorrect field types.
    Json(serde_json::Error),
    /// A profile failed semantic validation.
    InvalidProfile(String),
    /// Host key is not in the supported, bounded vocabulary.
    InvalidKey,
    /// Invalid viewport geometry.
    Geometry(ConfigError),
    /// A core contact operation could not be performed.
    Input(InputError),
}

impl fmt::Display for KeymapError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ProfileTooLarge => write!(f, "keymap exceeds {MAX_PROFILE_BYTES} bytes"),
            Self::Read(error) => write!(f, "could not read keymap: {error}"),
            Self::Json(error) => write!(f, "invalid keymap JSON: {error}"),
            Self::InvalidProfile(message) => f.write_str(message),
            Self::InvalidKey => f.write_str("unsupported host key name"),
            Self::Geometry(error) => error.fmt(f),
            Self::Input(error) => error.fmt(f),
        }
    }
}
impl Error for KeymapError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Read(error) => Some(error),
            Self::Json(error) => Some(error),
            Self::Geometry(error) => Some(error),
            Self::Input(error) => Some(error),
            _ => None,
        }
    }
}
impl From<InputError> for KeymapError {
    fn from(error: InputError) -> Self {
        Self::Input(error)
    }
}
impl From<ConfigError> for KeymapError {
    fn from(error: ConfigError) -> Self {
        Self::Geometry(error)
    }
}

/// A canonical logical host key. Names use lowercase ASCII, independent of case.
///
/// Supports `a`–`z`, `0`–`9`, `f1`–`f24`, `space`, `enter`, `tab`, `backspace`,
/// `delete`, `insert`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`, `left`,
/// `right`, `shift`, `control`, `alt`, and `escape`. Modifiers are individual keys,
/// not chord syntax. Keyboard layout/physical scancode mapping belongs to the host.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(try_from = "String", into = "String")]
pub struct HostKey(String);

impl HostKey {
    /// Validates a host key, accepting ASCII uppercase as the equivalent lowercase.
    pub fn new(value: &str) -> Result<Self, KeymapError> {
        if value.is_empty() || value.len() > 16 || !value.is_ascii() {
            return Err(KeymapError::InvalidKey);
        }
        let value = value.to_ascii_lowercase();
        let one = value.len() == 1 && value.as_bytes()[0].is_ascii_alphanumeric();
        let function = value
            .strip_prefix('f')
            .and_then(|n| n.parse::<u8>().ok())
            .is_some_and(|n| (1..=24).contains(&n) && value == format!("f{n}"));
        let named = matches!(
            value.as_str(),
            "space"
                | "enter"
                | "tab"
                | "backspace"
                | "delete"
                | "insert"
                | "home"
                | "end"
                | "pageup"
                | "pagedown"
                | "up"
                | "down"
                | "left"
                | "right"
                | "shift"
                | "control"
                | "alt"
                | "escape"
        );
        if one || function || named {
            Ok(Self(value))
        } else {
            Err(KeymapError::InvalidKey)
        }
    }

    /// Canonical lowercase logical key name.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}
impl FromStr for HostKey {
    type Err = KeymapError;
    fn from_str(value: &str) -> Result<Self, Self::Err> {
        Self::new(value)
    }
}
impl TryFrom<String> for HostKey {
    type Error = KeymapError;
    fn try_from(value: String) -> Result<Self, Self::Error> {
        Self::new(&value)
    }
}
impl From<HostKey> for String {
    fn from(value: HostKey) -> Self {
        value.0
    }
}

/// Supported window mouse buttons. Wheel/extra buttons are not contacts.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MouseButton {
    /// Primary button; reserved when direct pointer control is enabled.
    Left,
    /// Secondary button.
    Right,
    /// Middle button.
    Middle,
}

/// One key or mouse button; duplicate triggers across bindings are rejected.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Trigger {
    /// Logical keyboard key.
    Key {
        /// Canonical host key name.
        key: HostKey,
    },
    /// Window-scoped mouse button.
    Mouse {
        /// Button used to trigger the mapped action.
        button: MouseButton,
    },
}

/// Normalized coordinates, validated when a profile is loaded.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NormalizedPoint {
    /// Horizontal value in inclusive `[0, 1]`.
    pub x: f64,
    /// Vertical value in inclusive `[0, 1]`.
    pub y: f64,
}

impl NormalizedPoint {
    fn valid(self) -> bool {
        self.x.is_finite()
            && self.y.is_finite()
            && (0.0..=1.0).contains(&self.x)
            && (0.0..=1.0).contains(&self.y)
    }
}

/// One normalized tap, hold, or four-key directional joystick.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Binding {
    /// Touch down/up once on a fresh press; held/repeat events cannot retrigger.
    Tap {
        /// Source key or button.
        trigger: Trigger,
        /// Touch location.
        point: NormalizedPoint,
    },
    /// Keep a touch active until source release or lifecycle cancellation.
    Hold {
        /// Source key or button.
        trigger: Trigger,
        /// Touch location.
        point: NormalizedPoint,
    },
    /// One contact shared by four directional keys, with normalized diagonals.
    /// Opposite keys cancel their axis. All keys up releases the contact.
    Joystick {
        /// Negative vertical direction.
        up: HostKey,
        /// Negative horizontal direction.
        left: HostKey,
        /// Positive vertical direction.
        down: HostKey,
        /// Positive horizontal direction.
        right: HostKey,
        /// Initial touch and neutral position.
        center: NormalizedPoint,
        /// Positive normalized radius; the entire circle must fit the unit square.
        radius: f64,
    },
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawProfile {
    version: u32,
    name: String,
    #[serde(default)]
    pointer: bool,
    bindings: Vec<Binding>,
}

/// An immutable, fully validated versioned profile.
#[derive(Debug, Clone, Serialize)]
pub struct Keymap {
    version: u32,
    name: String,
    pointer: bool,
    bindings: Vec<Binding>,
}

impl Keymap {
    /// Parses a bounded profile; validates all fields before exposing any state.
    pub fn from_json(json: &str) -> Result<Self, KeymapError> {
        Self::from_bytes(json.as_bytes())
    }

    /// Reads at most the profile limit plus one byte, then validates the complete
    /// UTF-8 JSON. Use for caller-opened profile files to avoid unbounded file reads.
    /// Opening a path and choosing when to read are explicitly the caller's job.
    pub fn from_reader(reader: impl Read) -> Result<Self, KeymapError> {
        let mut bytes = Vec::new();
        reader
            .take(MAX_PROFILE_BYTES as u64 + 1)
            .read_to_end(&mut bytes)
            .map_err(KeymapError::Read)?;
        Self::from_bytes(&bytes)
    }

    fn from_bytes(bytes: &[u8]) -> Result<Self, KeymapError> {
        if bytes.len() > MAX_PROFILE_BYTES {
            return Err(KeymapError::ProfileTooLarge);
        }
        let raw: RawProfile = serde_json::from_slice(bytes).map_err(KeymapError::Json)?;
        let invalid = |message: &str| KeymapError::InvalidProfile(message.to_owned());
        if raw.version != PROFILE_VERSION {
            return Err(invalid("unsupported keymap version; expected 1"));
        }
        if raw.name.is_empty() || raw.name.len() > 80 || raw.name.chars().any(char::is_control) {
            return Err(invalid(
                "keymap name must have 1..=80 bytes and no control characters",
            ));
        }
        let owners = raw.bindings.len() + usize::from(raw.pointer);
        if !(1..=MAX_BINDINGS).contains(&owners) {
            return Err(invalid(
                "keymap must have 1..=32 contact owners, including the pointer",
            ));
        }
        let mut triggers = HashSet::new();
        if raw.pointer {
            triggers.insert(Trigger::Mouse {
                button: MouseButton::Left,
            });
        }
        for binding in &raw.bindings {
            let sources = match binding {
                Binding::Tap { trigger, point } | Binding::Hold { trigger, point } => {
                    if !point.valid() {
                        return Err(invalid("touch points must be finite and inside [0, 1]"));
                    }
                    vec![trigger.clone()]
                }
                Binding::Joystick {
                    up,
                    left,
                    down,
                    right,
                    center,
                    radius,
                } => {
                    if !center.valid()
                        || !radius.is_finite()
                        || *radius <= 0.0
                        || center.x - radius < 0.0
                        || center.x + radius > 1.0
                        || center.y - radius < 0.0
                        || center.y + radius > 1.0
                    {
                        return Err(invalid(
                            "joystick radius must be positive and fit around its center in [0, 1]",
                        ));
                    }
                    [up, left, down, right]
                        .into_iter()
                        .map(|key| Trigger::Key { key: key.clone() })
                        .collect()
                }
            };
            for trigger in sources {
                if matches!(&trigger, Trigger::Key { key } if key.as_str() == "escape") {
                    return Err(invalid("escape is reserved for emergency release"));
                }
                if !triggers.insert(trigger) {
                    return Err(invalid("each key or button may occur only once; left mouse is reserved when pointer is enabled"));
                }
            }
        }
        Ok(Self {
            version: raw.version,
            name: raw.name,
            pointer: raw.pointer,
            bindings: raw.bindings,
        })
    }

    /// Serializes the same validated schema for an explicit caller-managed save.
    pub fn to_json(&self) -> Result<String, KeymapError> {
        serde_json::to_string_pretty(self).map_err(KeymapError::Json)
    }

    /// Human-readable profile name.
    pub fn name(&self) -> &str {
        &self.name
    }
    /// Whether primary mouse button controls a direct touch within the viewport.
    pub fn pointer_enabled(&self) -> bool {
        self.pointer
    }
    /// Validated bindings, in stable contact-identity order.
    pub fn bindings(&self) -> &[Binding] {
        &self.bindings
    }
    /// Slot count sufficient for every binding and pointer to be simultaneously active.
    pub fn max_contacts(&self) -> usize {
        self.bindings.len() + usize::from(self.pointer)
    }
}

impl Default for Keymap {
    fn default() -> Self {
        Self::from_json(DEFAULT_KEYMAP_JSON).expect("compiled default keymap is valid")
    }
}

/// Validated content rectangle using the same units as host mouse events.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WindowViewport {
    left: f64,
    top: f64,
    width: f64,
    height: f64,
    right: f64,
    bottom: f64,
    core: Viewport,
}
impl WindowViewport {
    /// Validates finite positive dimensions and representable edges.
    pub fn new(left: f64, top: f64, width: f64, height: f64) -> Result<Self, KeymapError> {
        let core = Viewport::new(left, top, width, height)?;
        let right = left + width;
        let bottom = top + height;
        Ok(Self {
            left,
            top,
            width: right - left,
            height: bottom - top,
            right,
            bottom,
            core,
        })
    }
    /// Left edge in host event units.
    pub fn left(self) -> f64 {
        self.left
    }
    /// Top edge in host event units.
    pub fn top(self) -> f64 {
        self.top
    }
    /// Effective representable width.
    pub fn width(self) -> f64 {
        self.width
    }
    /// Effective representable height.
    pub fn height(self) -> f64 {
        self.height
    }
    fn contains(self, x: f64, y: f64) -> bool {
        x.is_finite()
            && y.is_finite()
            && x >= self.left
            && x <= self.right
            && y >= self.top
            && y <= self.bottom
    }
    fn point(self, point: NormalizedPoint) -> (f64, f64) {
        (
            self.left + point.x * self.width,
            self.top + point.y * self.height,
        )
    }
    fn mapping(self, size: AndroidSize, rotation: Rotation) -> CoordinateMapping {
        CoordinateMapping::new(self.core, size, rotation, BoundsPolicy::Clamp)
    }
}

/// Events delivered only by the owning window. No event implies global capture.
#[derive(Debug, Clone, PartialEq)]
pub enum HostEvent {
    /// A logical key press. Repeat events never initiate or retrigger a contact.
    KeyDown {
        /// Logical key from the current window.
        key: HostKey,
        /// Host auto-repeat flag; true is always ignored.
        repeat: bool,
    },
    /// Logical key release; duplicate or stale releases are harmless.
    KeyUp {
        /// Released logical key.
        key: HostKey,
    },
    /// Window mouse button press, accepted only inside the content rectangle.
    MouseDown {
        /// Pressed mouse button.
        button: MouseButton,
        /// Horizontal host coordinate.
        x: f64,
        /// Vertical host coordinate.
        y: f64,
    },
    /// Movement of the active direct pointer; finite outside points clamp to edges.
    MouseMove {
        /// Horizontal host coordinate.
        x: f64,
        /// Vertical host coordinate.
        y: f64,
    },
    /// Mouse release, without requiring a still-valid coordinate.
    MouseUp {
        /// Released mouse button.
        button: MouseButton,
    },
    /// Release all contacts and gate input until a new focus-gained event.
    FocusLost,
    /// Explicitly enable window input. Does not restore previously held contacts.
    FocusGained,
    /// Cancel every contact and gate input until explicitly focused again.
    /// A non-repeat Escape key down is equivalent.
    EmergencyRelease,
}

#[derive(Debug, Clone, Copy, Default)]
struct BindingState {
    pressed: bool,
    directions: [bool; 4],
}

/// Serial window-input state with bounded simultaneous contacts.
///
/// The host must deliver focus loss, resize, emergency release, and shutdown cleanup.
/// The controller starts unfocused; Escape releases and disables it. Key repeat is
/// never a fresh press, including after a resize/focus interruption. Profiles cannot
/// change in place: release the old controller/adapter before constructing another.
#[derive(Debug)]
pub struct KeymapController {
    keymap: Keymap,
    viewport: WindowViewport,
    size: AndroidSize,
    rotation: Rotation,
    allocator: ContactAllocator,
    states: Vec<BindingState>,
    focused: bool,
    pointer_active: bool,
}

impl KeymapController {
    /// Constructs an empty, unfocused controller with profile-sized capacity.
    pub fn new(
        keymap: Keymap,
        viewport: WindowViewport,
        size: AndroidSize,
        rotation: Rotation,
    ) -> Result<Self, KeymapError> {
        let allocator =
            ContactAllocator::new(keymap.max_contacts(), viewport.mapping(size, rotation))?;
        let states = vec![BindingState::default(); keymap.bindings.len()];
        Ok(Self {
            keymap,
            viewport,
            size,
            rotation,
            allocator,
            states,
            focused: false,
            pointer_active: false,
        })
    }
    /// Current immutable profile.
    pub fn keymap(&self) -> &Keymap {
        &self.keymap
    }
    /// Number of active source contacts; this does not prove downstream delivery.
    pub fn active_count(&self) -> usize {
        self.allocator.active_count()
    }
    /// Whether host events currently have permission to initiate contacts.
    pub fn focused(&self) -> bool {
        self.focused
    }
    /// Required adapter slot capacity.
    pub fn max_contacts(&self) -> usize {
        self.allocator.capacity()
    }
    /// Current destination coordinate-space size.
    pub fn android_size(&self) -> AndroidSize {
        self.size
    }
    /// Current content rectangle.
    pub fn viewport(&self) -> WindowViewport {
        self.viewport
    }

    /// Cancels contacts at old coordinates before replacing the content rectangle.
    /// Focus stays as-is; held repeat events cannot recreate canceled contacts.
    #[must_use = "deliver every cleanup transition before new input"]
    pub fn set_viewport(&mut self, viewport: WindowViewport) -> Vec<ContactTransition> {
        self.set_geometry(viewport, self.size, self.rotation)
    }

    /// Cancels contacts before applying content, destination, or rotation changes.
    /// Destination dimension changes require reconfiguring/replacing the adapter.
    #[must_use = "deliver every cleanup transition before new input"]
    pub fn set_geometry(
        &mut self,
        viewport: WindowViewport,
        size: AndroidSize,
        rotation: Rotation,
    ) -> Vec<ContactTransition> {
        let events = self.allocator.set_mapping(viewport.mapping(size, rotation));
        self.reset_sources();
        self.viewport = viewport;
        self.size = size;
        self.rotation = rotation;
        events
    }

    /// Translates one window event. Deliver returned transitions in order, each in
    /// its own adapter frame. Empty output has no device effect. Unknown keys, repeat
    /// downs, duplicate ups, and input while unfocused are harmless no-ops.
    pub fn handle(&mut self, event: HostEvent) -> Result<Vec<ContactTransition>, KeymapError> {
        match event {
            HostEvent::FocusLost => {
                self.focused = false;
                self.reset_sources();
                return Ok(self.allocator.focus_lost());
            }
            HostEvent::EmergencyRelease => {
                self.focused = false;
                self.reset_sources();
                return Ok(self.allocator.cancel_all());
            }
            HostEvent::FocusGained => {
                self.focused = true;
                return Ok(Vec::new());
            }
            _ => {}
        }
        if !self.focused {
            return Ok(Vec::new());
        }
        match event {
            HostEvent::KeyDown { key, repeat } => {
                if repeat {
                    return Ok(Vec::new());
                }
                if key.as_str() == "escape" {
                    return self.handle(HostEvent::EmergencyRelease);
                }
                self.trigger(Trigger::Key { key }, true)
            }
            HostEvent::KeyUp { key } => self.trigger(Trigger::Key { key }, false),
            HostEvent::MouseDown { button, x, y } => {
                if !self.viewport.contains(x, y) {
                    return Ok(Vec::new());
                }
                if self.keymap.pointer && button == MouseButton::Left {
                    if self.pointer_active {
                        return Ok(Vec::new());
                    }
                    let down = self.allocator.down(self.pointer_id(), x, y)?;
                    self.pointer_active = true;
                    Ok(vec![down])
                } else {
                    self.trigger(Trigger::Mouse { button }, true)
                }
            }
            HostEvent::MouseUp { button } => {
                if self.keymap.pointer && button == MouseButton::Left {
                    self.pointer_active = false;
                    Ok(self.allocator.up(self.pointer_id()).into_iter().collect())
                } else {
                    self.trigger(Trigger::Mouse { button }, false)
                }
            }
            HostEvent::MouseMove { x, y } => {
                if !self.pointer_active {
                    return Ok(Vec::new());
                }
                Ok(self
                    .allocator
                    .move_to(self.pointer_id(), x, y)?
                    .into_iter()
                    .collect())
            }
            HostEvent::FocusLost | HostEvent::FocusGained | HostEvent::EmergencyRelease => {
                unreachable!("lifecycle events handled first")
            }
        }
    }

    fn reset_sources(&mut self) {
        self.states.fill(BindingState::default());
        self.pointer_active = false;
    }
    fn pointer_id(&self) -> ContactId {
        ContactId(self.keymap.bindings.len() as u64)
    }

    fn trigger(
        &mut self,
        source: Trigger,
        down: bool,
    ) -> Result<Vec<ContactTransition>, KeymapError> {
        for (index, binding) in self.keymap.bindings.iter().enumerate() {
            let id = ContactId(index as u64);
            let state = &mut self.states[index];
            match binding {
                Binding::Tap { trigger, point } | Binding::Hold { trigger, point }
                    if *trigger == source =>
                {
                    if state.pressed == down {
                        return Ok(Vec::new());
                    }
                    if down {
                        let (x, y) = self.viewport.point(*point);
                        let begin = self.allocator.down(id, x, y)?;
                        state.pressed = true;
                        let mut events = vec![begin];
                        if matches!(binding, Binding::Tap { .. }) {
                            events.extend(self.allocator.up(id));
                        }
                        return Ok(events);
                    }
                    state.pressed = false;
                    return Ok(self.allocator.up(id).into_iter().collect());
                }
                Binding::Joystick {
                    up,
                    left,
                    down: bottom,
                    right,
                    center,
                    radius,
                } => {
                    let Trigger::Key { key } = &source else {
                        continue;
                    };
                    let Some(direction) = [up, left, bottom, right]
                        .iter()
                        .position(|candidate| *candidate == key)
                    else {
                        continue;
                    };
                    if state.directions[direction] == down {
                        return Ok(Vec::new());
                    }
                    let mut next = state.directions;
                    next[direction] = down;
                    if !next.iter().any(|pressed| *pressed) {
                        state.directions = next;
                        return Ok(self.allocator.up(id).into_iter().collect());
                    }
                    let dx = i32::from(next[3]) - i32::from(next[1]);
                    let dy = i32::from(next[2]) - i32::from(next[0]);
                    let length = f64::from(dx * dx + dy * dy).sqrt().max(1.0);
                    let point = NormalizedPoint {
                        x: center.x + radius * f64::from(dx) / length,
                        y: center.y + radius * f64::from(dy) / length,
                    };
                    let mut events = Vec::with_capacity(2);
                    if self.allocator.contact(id).is_none() {
                        let (x, y) = self.viewport.point(*center);
                        events.push(self.allocator.down(id, x, y)?);
                    }
                    let (x, y) = self.viewport.point(point);
                    // The ID is active and validated center/radius guarantee a finite point.
                    events.extend(self.allocator.move_to(id, x, y)?);
                    state.directions = next;
                    return Ok(events);
                }
                _ => {}
            }
        }
        Ok(Vec::new())
    }
}
