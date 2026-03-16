//! Virtio input device backend
//!
//! Implements the virtio-input device as defined in the virtio specification
//! (Section 5.8). Provides touchscreen, keyboard, and multi-touch input
//! to the guest using the Linux evdev event model.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::VecDeque;

// ============================================================================
// Linux input event types (from <linux/input-event-codes.h>)
// ============================================================================

pub mod ev_types {
    /// Synchronization event
    pub const EV_SYN: u16 = 0x00;
    /// Key/button event
    pub const EV_KEY: u16 = 0x01;
    /// Relative axis event
    pub const EV_REL: u16 = 0x02;
    /// Absolute axis event
    pub const EV_ABS: u16 = 0x03;
}

pub mod syn_codes {
    /// Separator between event groups
    pub const SYN_REPORT: u16 = 0;
    /// Indicates slots are being reported
    pub const SYN_MT_REPORT: u16 = 2;
}

pub mod key_codes {
    pub const KEY_RESERVED: u16 = 0;
    pub const KEY_ESC: u16 = 1;
    pub const KEY_1: u16 = 2;
    pub const KEY_2: u16 = 3;
    pub const KEY_3: u16 = 4;
    pub const KEY_4: u16 = 5;
    pub const KEY_5: u16 = 6;
    pub const KEY_6: u16 = 7;
    pub const KEY_7: u16 = 8;
    pub const KEY_8: u16 = 9;
    pub const KEY_9: u16 = 10;
    pub const KEY_0: u16 = 11;
    pub const KEY_Q: u16 = 16;
    pub const KEY_W: u16 = 17;
    pub const KEY_E: u16 = 18;
    pub const KEY_R: u16 = 19;
    pub const KEY_T: u16 = 20;
    pub const KEY_Y: u16 = 21;
    pub const KEY_A: u16 = 30;
    pub const KEY_S: u16 = 31;
    pub const KEY_D: u16 = 32;
    pub const KEY_ENTER: u16 = 28;
    pub const KEY_SPACE: u16 = 57;
    pub const KEY_BACKSPACE: u16 = 14;
    pub const KEY_TAB: u16 = 15;
    pub const KEY_UP: u16 = 103;
    pub const KEY_DOWN: u16 = 108;
    pub const KEY_LEFT: u16 = 105;
    pub const KEY_RIGHT: u16 = 106;
    pub const KEY_HOME: u16 = 102;
    pub const KEY_BACK: u16 = 158;
    pub const KEY_POWER: u16 = 116;
    pub const KEY_VOLUMEUP: u16 = 115;
    pub const KEY_VOLUMEDOWN: u16 = 114;
    /// Touch button
    pub const BTN_TOUCH: u16 = 0x14a;
    /// Tool type finger (for touch)
    pub const BTN_TOOL_FINGER: u16 = 0x145;
    /// Maximum key code we report
    pub const KEY_MAX: u16 = 0x2ff;
}

pub mod abs_codes {
    pub const ABS_X: u16 = 0x00;
    pub const ABS_Y: u16 = 0x01;
    pub const ABS_PRESSURE: u16 = 0x18;
    /// Multi-touch slot
    pub const ABS_MT_SLOT: u16 = 0x2f;
    /// Multi-touch touch major axis
    pub const ABS_MT_TOUCH_MAJOR: u16 = 0x30;
    /// Multi-touch X position
    pub const ABS_MT_POSITION_X: u16 = 0x35;
    /// Multi-touch Y position
    pub const ABS_MT_POSITION_Y: u16 = 0x36;
    /// Multi-touch tracking ID
    pub const ABS_MT_TRACKING_ID: u16 = 0x39;
    /// Multi-touch pressure
    pub const ABS_MT_PRESSURE: u16 = 0x3a;
    /// Maximum ABS code
    pub const ABS_MAX: u16 = 0x3f;
}

// ============================================================================
// Virtio input config selectors (virtio spec 5.8.2)
// ============================================================================

pub mod config_select {
    /// No selection
    pub const VIRTIO_INPUT_CFG_UNSET: u8 = 0x00;
    /// subsel: 0, returns name string
    pub const VIRTIO_INPUT_CFG_ID_NAME: u8 = 0x01;
    /// subsel: 0, returns serial number string
    pub const VIRTIO_INPUT_CFG_ID_SERIAL: u8 = 0x02;
    /// subsel: 0, returns device IDs (struct virtio_input_devids)
    pub const VIRTIO_INPUT_CFG_ID_DEVIDS: u8 = 0x03;
    /// subsel: 0, returns property bits
    pub const VIRTIO_INPUT_CFG_PROP_BITS: u8 = 0x10;
    /// subsel: event type, returns event code bits
    pub const VIRTIO_INPUT_CFG_EV_BITS: u8 = 0x11;
    /// subsel: abs axis, returns abs info
    pub const VIRTIO_INPUT_CFG_ABS_INFO: u8 = 0x12;
}

// ============================================================================
// Input event (matches Linux struct input_event, 8 bytes in virtio)
// ============================================================================

/// Size of a virtio input event in bytes (without timestamp, per virtio spec)
pub const VIRTIO_INPUT_EVENT_SIZE: usize = 8;

/// Input event matching the virtio-input event structure.
///
/// Note: virtio-input uses a simplified format without the timestamp
/// fields present in Linux's struct input_event.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct InputEvent {
    /// Event type (EV_SYN, EV_KEY, EV_ABS, etc.)
    pub type_: u16,
    /// Event code (key code, axis code, etc.)
    pub code: u16,
    /// Event value (key state, axis position, etc.)
    pub value: i32,
}

impl InputEvent {
    /// Create a new input event
    pub fn new(type_: u16, code: u16, value: i32) -> Self {
        Self { type_, code, value }
    }

    /// Create a SYN_REPORT event (marks end of a group of events)
    pub fn syn_report() -> Self {
        Self::new(ev_types::EV_SYN, syn_codes::SYN_REPORT, 0)
    }

    /// Serialize to bytes (little-endian, 8 bytes per virtio spec)
    pub fn to_bytes(&self) -> [u8; VIRTIO_INPUT_EVENT_SIZE] {
        let mut buf = [0u8; VIRTIO_INPUT_EVENT_SIZE];
        buf[0..2].copy_from_slice(&self.type_.to_le_bytes());
        buf[2..4].copy_from_slice(&self.code.to_le_bytes());
        buf[4..8].copy_from_slice(&self.value.to_le_bytes());
        buf
    }

    /// Parse from bytes (little-endian)
    pub fn from_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < VIRTIO_INPUT_EVENT_SIZE {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            type_: u16::from_le_bytes([buf[0], buf[1]]),
            code: u16::from_le_bytes([buf[2], buf[3]]),
            value: i32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]),
        })
    }
}

// ============================================================================
// Absolute axis info (matches Linux struct input_absinfo)
// ============================================================================

/// Absolute axis information, returned for VIRTIO_INPUT_CFG_ABS_INFO queries.
#[derive(Debug, Clone, Copy)]
pub struct AbsInfo {
    /// Minimum value
    pub min: i32,
    /// Maximum value
    pub max: i32,
    /// Fuzz (noise filter)
    pub fuzz: i32,
    /// Flat (dead zone)
    pub flat: i32,
    /// Resolution (units per mm)
    pub res: i32,
}

impl AbsInfo {
    /// Serialize to bytes (little-endian, 20 bytes)
    pub fn to_bytes(&self) -> [u8; 20] {
        let mut buf = [0u8; 20];
        buf[0..4].copy_from_slice(&self.min.to_le_bytes());
        buf[4..8].copy_from_slice(&self.max.to_le_bytes());
        buf[8..12].copy_from_slice(&self.fuzz.to_le_bytes());
        buf[12..16].copy_from_slice(&self.flat.to_le_bytes());
        buf[16..20].copy_from_slice(&self.res.to_le_bytes());
        buf
    }
}

// ============================================================================
// Device IDs
// ============================================================================

/// Device identification (returned for VIRTIO_INPUT_CFG_ID_DEVIDS)
#[derive(Debug, Clone, Copy)]
pub struct InputDevIds {
    /// Bus type (BUS_VIRTUAL = 0x06)
    pub bustype: u16,
    /// Vendor ID
    pub vendor: u16,
    /// Product ID
    pub product: u16,
    /// Version
    pub version: u16,
}

impl InputDevIds {
    /// Serialize to bytes (little-endian, 8 bytes)
    pub fn to_bytes(&self) -> [u8; 8] {
        let mut buf = [0u8; 8];
        buf[0..2].copy_from_slice(&self.bustype.to_le_bytes());
        buf[2..4].copy_from_slice(&self.vendor.to_le_bytes());
        buf[4..6].copy_from_slice(&self.product.to_le_bytes());
        buf[6..8].copy_from_slice(&self.version.to_le_bytes());
        buf
    }
}

// ============================================================================
// Input device subtype
// ============================================================================

/// Specifies the type of input device to emulate
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputDeviceKind {
    /// Multi-touch touchscreen
    Touchscreen,
    /// Full keyboard
    Keyboard,
}

// ============================================================================
// Virtqueue indices
// ============================================================================

/// Event queue (device -> driver): delivers input events to the guest
pub const EVENTQ: u16 = 0;
/// Status queue (driver -> device): guest reports LED/feedback status
pub const STATUSQ: u16 = 1;

// ============================================================================
// Touchscreen configuration
// ============================================================================

/// Configuration for a touchscreen device
#[derive(Debug, Clone)]
pub struct TouchscreenConfig {
    /// Screen width in pixels
    pub width: u32,
    /// Screen height in pixels
    pub height: u32,
    /// Maximum number of simultaneous touch points
    pub max_contacts: u32,
}

impl Default for TouchscreenConfig {
    fn default() -> Self {
        Self {
            width: 1080,
            height: 1920,
            max_contacts: 10,
        }
    }
}

// ============================================================================
// VirtioInput device
// ============================================================================

/// Virtio input device.
///
/// Implements the virtio-input protocol with support for touchscreen
/// and keyboard devices. Events are queued via the injection API and
/// delivered to the guest through virtqueue 0 (the event queue).
pub struct VirtioInput {
    /// Device kind (touchscreen or keyboard)
    kind: InputDeviceKind,
    /// Device name
    name: String,
    /// Device serial number
    serial: String,
    /// Device identification
    devids: InputDevIds,
    /// Touchscreen configuration (only for touchscreen devices)
    touch_config: Option<TouchscreenConfig>,
    /// Event queue: pending events to deliver to the guest
    event_queue: VecDeque<InputEvent>,
    /// Feature flags (virtio-input has no feature bits in the spec)
    features: u64,
    /// Whether the device has been activated
    activated: bool,
    /// Absolute axis info table (axis code -> AbsInfo)
    abs_info: Vec<(u16, AbsInfo)>,
    /// Supported event types as a bitmask
    supported_ev_types: u32,
    /// Supported key codes as a bitmask (represented as Vec for flexibility)
    supported_keys: Vec<u16>,
    /// Supported ABS codes
    supported_abs: Vec<u16>,
}

impl VirtioInput {
    /// Create a new touchscreen input device
    pub fn new_touchscreen(config: TouchscreenConfig) -> Self {
        let mut abs_info = Vec::new();
        let mut supported_abs = Vec::new();

        // Single-touch axes
        abs_info.push((abs_codes::ABS_X, AbsInfo {
            min: 0,
            max: config.width as i32 - 1,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_Y, AbsInfo {
            min: 0,
            max: config.height as i32 - 1,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));

        // Multi-touch axes
        abs_info.push((abs_codes::ABS_MT_POSITION_X, AbsInfo {
            min: 0,
            max: config.width as i32 - 1,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_MT_POSITION_Y, AbsInfo {
            min: 0,
            max: config.height as i32 - 1,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_MT_TRACKING_ID, AbsInfo {
            min: 0,
            max: config.max_contacts as i32 - 1,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_MT_SLOT, AbsInfo {
            min: 0,
            max: config.max_contacts as i32 - 1,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_MT_TOUCH_MAJOR, AbsInfo {
            min: 0,
            max: 255,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_MT_PRESSURE, AbsInfo {
            min: 0,
            max: 255,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));
        abs_info.push((abs_codes::ABS_PRESSURE, AbsInfo {
            min: 0,
            max: 255,
            fuzz: 0,
            flat: 0,
            res: 0,
        }));

        supported_abs.extend_from_slice(&[
            abs_codes::ABS_X,
            abs_codes::ABS_Y,
            abs_codes::ABS_PRESSURE,
            abs_codes::ABS_MT_SLOT,
            abs_codes::ABS_MT_TOUCH_MAJOR,
            abs_codes::ABS_MT_POSITION_X,
            abs_codes::ABS_MT_POSITION_Y,
            abs_codes::ABS_MT_TRACKING_ID,
            abs_codes::ABS_MT_PRESSURE,
        ]);

        let supported_keys = vec![
            key_codes::BTN_TOUCH,
            key_codes::BTN_TOOL_FINGER,
        ];

        // EV_SYN | EV_KEY | EV_ABS
        let supported_ev_types = (1 << ev_types::EV_SYN)
            | (1 << ev_types::EV_KEY)
            | (1 << ev_types::EV_ABS);

        Self {
            kind: InputDeviceKind::Touchscreen,
            name: "RexPlayer Touchscreen".to_string(),
            serial: "rex-touch-001".to_string(),
            devids: InputDevIds {
                bustype: 0x06, // BUS_VIRTUAL
                vendor: 0x5245,  // "RE"
                product: 0x5401, // "T\x01"
                version: 1,
            },
            touch_config: Some(config),
            event_queue: VecDeque::with_capacity(256),
            features: 0,
            activated: false,
            abs_info,
            supported_ev_types,
            supported_keys,
            supported_abs,
        }
    }

    /// Create a new keyboard input device
    pub fn new_keyboard() -> Self {
        // Standard keyboard keys
        let mut supported_keys = Vec::new();
        // Add common key codes
        for code in 1..=key_codes::KEY_MAX {
            // Add a reasonable subset of keys (all standard keys up to 248)
            if code <= 248 || code == key_codes::KEY_BACK {
                supported_keys.push(code);
            }
        }

        // EV_SYN | EV_KEY
        let supported_ev_types = (1 << ev_types::EV_SYN) | (1 << ev_types::EV_KEY);

        Self {
            kind: InputDeviceKind::Keyboard,
            name: "RexPlayer Keyboard".to_string(),
            serial: "rex-kbd-001".to_string(),
            devids: InputDevIds {
                bustype: 0x06, // BUS_VIRTUAL
                vendor: 0x5245,  // "RE"
                product: 0x4B01, // "K\x01"
                version: 1,
            },
            touch_config: None,
            event_queue: VecDeque::with_capacity(256),
            features: 0,
            activated: false,
            abs_info: Vec::new(),
            supported_ev_types,
            supported_keys,
            supported_abs: Vec::new(),
        }
    }

    /// Get the device kind
    pub fn kind(&self) -> InputDeviceKind {
        self.kind
    }

    /// Get the touchscreen configuration if this device is a touchscreen.
    pub fn touchscreen_config(&self) -> Option<&TouchscreenConfig> {
        self.touch_config.as_ref()
    }

    /// Get the device name
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Get the number of pending events
    pub fn pending_events(&self) -> usize {
        self.event_queue.len()
    }

    /// Inject a raw input event into the event queue
    pub fn inject_event(&mut self, event: InputEvent) {
        self.event_queue.push_back(event);
    }

    /// Inject a single-touch event (touch down, move, or lift).
    ///
    /// - `x`, `y`: touch coordinates
    /// - `pressure`: 0 = lift, >0 = touch/move
    pub fn inject_touch(&mut self, x: i32, y: i32, pressure: i32) {
        if pressure > 0 {
            // Touch down or move
            self.inject_event(InputEvent::new(
                ev_types::EV_KEY,
                key_codes::BTN_TOUCH,
                1,
            ));
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_X,
                x,
            ));
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_Y,
                y,
            ));
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_PRESSURE,
                pressure,
            ));
        } else {
            // Touch lift
            self.inject_event(InputEvent::new(
                ev_types::EV_KEY,
                key_codes::BTN_TOUCH,
                0,
            ));
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_PRESSURE,
                0,
            ));
        }
        // SYN_REPORT marks end of this event group
        self.inject_event(InputEvent::syn_report());
    }

    /// Inject a multi-touch event for a specific slot/finger.
    ///
    /// - `slot`: touch slot (finger index)
    /// - `tracking_id`: tracking ID (-1 to lift)
    /// - `x`, `y`: coordinates
    /// - `pressure`: touch pressure
    pub fn inject_multitouch(
        &mut self,
        slot: i32,
        tracking_id: i32,
        x: i32,
        y: i32,
        pressure: i32,
    ) {
        self.inject_event(InputEvent::new(
            ev_types::EV_ABS,
            abs_codes::ABS_MT_SLOT,
            slot,
        ));

        self.inject_event(InputEvent::new(
            ev_types::EV_ABS,
            abs_codes::ABS_MT_TRACKING_ID,
            tracking_id,
        ));

        if tracking_id >= 0 {
            // Finger is down or moving
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_MT_POSITION_X,
                x,
            ));
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_MT_POSITION_Y,
                y,
            ));
            self.inject_event(InputEvent::new(
                ev_types::EV_ABS,
                abs_codes::ABS_MT_PRESSURE,
                pressure,
            ));
        }

        self.inject_event(InputEvent::syn_report());
    }

    /// Inject a key press or release event.
    ///
    /// - `code`: key code (from key_codes module)
    /// - `value`: 0 = release, 1 = press, 2 = repeat
    pub fn inject_key(&mut self, code: u16, value: i32) {
        self.inject_event(InputEvent::new(ev_types::EV_KEY, code, value));
        self.inject_event(InputEvent::syn_report());
    }

    /// Pop the next event from the event queue for delivery to the guest
    pub fn pop_event(&mut self) -> Option<InputEvent> {
        self.event_queue.pop_front()
    }

    /// Query the device configuration for a given selector and sub-selector.
    ///
    /// This implements the virtio-input config space query mechanism where
    /// the driver writes (select, subsel) and reads back the result.
    pub fn query_config(&self, select: u8, subsel: u8) -> Vec<u8> {
        match select {
            config_select::VIRTIO_INPUT_CFG_ID_NAME => {
                self.name.as_bytes().to_vec()
            }

            config_select::VIRTIO_INPUT_CFG_ID_SERIAL => {
                self.serial.as_bytes().to_vec()
            }

            config_select::VIRTIO_INPUT_CFG_ID_DEVIDS => {
                self.devids.to_bytes().to_vec()
            }

            config_select::VIRTIO_INPUT_CFG_PROP_BITS => {
                // Property bits — for touchscreen, we set INPUT_PROP_DIRECT (0x01)
                match self.kind {
                    InputDeviceKind::Touchscreen => vec![0x01], // INPUT_PROP_DIRECT
                    InputDeviceKind::Keyboard => vec![0x00],
                }
            }

            config_select::VIRTIO_INPUT_CFG_EV_BITS => {
                // subsel = event type; return bitmask of supported codes
                let event_mask = 1u32
                    .checked_shl(u32::from(subsel))
                    .unwrap_or(0);
                if event_mask == 0 || self.supported_ev_types & event_mask == 0 {
                    return Vec::new();
                }

                match subsel {
                    0 => {
                        // EV_SYN: always supported
                        vec![0x01] // bit 0 = SYN_REPORT
                    }
                    t if t == ev_types::EV_KEY as u8 => {
                        // Return bitmask of supported key codes
                        self.key_bits()
                    }
                    t if t == ev_types::EV_ABS as u8 => {
                        // Return bitmask of supported ABS codes
                        self.abs_bits()
                    }
                    t if t == ev_types::EV_REL as u8 => {
                        Vec::new() // no REL support
                    }
                    _ => Vec::new(),
                }
            }

            config_select::VIRTIO_INPUT_CFG_ABS_INFO => {
                // subsel = ABS axis code; return AbsInfo
                for &(code, ref info) in &self.abs_info {
                    if code == subsel as u16 {
                        return info.to_bytes().to_vec();
                    }
                }
                Vec::new()
            }

            _ => Vec::new(),
        }
    }

    /// Build a bitmask of supported key codes
    fn key_bits(&self) -> Vec<u8> {
        if self.supported_keys.is_empty() {
            return Vec::new();
        }

        let max_code = self.supported_keys.iter().copied().max().unwrap_or(0);
        let num_bytes = (max_code as usize / 8) + 1;
        let mut bits = vec![0u8; num_bytes];

        for &code in &self.supported_keys {
            let byte_idx = code as usize / 8;
            let bit_idx = code as usize % 8;
            if byte_idx < bits.len() {
                bits[byte_idx] |= 1 << bit_idx;
            }
        }

        bits
    }

    /// Build a bitmask of supported ABS codes
    fn abs_bits(&self) -> Vec<u8> {
        if self.supported_abs.is_empty() {
            return Vec::new();
        }

        let max_code = self.supported_abs.iter().copied().max().unwrap_or(0);
        let num_bytes = (max_code as usize / 8) + 1;
        let mut bits = vec![0u8; num_bytes];

        for &code in &self.supported_abs {
            let byte_idx = code as usize / 8;
            let bit_idx = code as usize % 8;
            if byte_idx < bits.len() {
                bits[byte_idx] |= 1 << bit_idx;
            }
        }

        bits
    }

    /// Get the AbsInfo for a given axis code
    pub fn get_abs_info(&self, code: u16) -> Option<&AbsInfo> {
        self.abs_info.iter().find(|(c, _)| *c == code).map(|(_, info)| info)
    }
}

impl VirtioDevice for VirtioInput {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Input
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!(
            "virtio-input activated: {:?} ({})",
            self.kind,
            self.name
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.event_queue.clear();
    }

    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
        match queue_index {
            EVENTQ => {
                // Event queue: deliver pending input events to guest buffers.
                // The transport layer reads events via pop_event() and writes
                // them into the guest-provided buffers.
                tracing::trace!(
                    "virtio-input: event queue notified, {} events pending",
                    self.event_queue.len()
                );
                Ok(())
            }
            STATUSQ => {
                // Status queue: guest reports LED state, etc.
                // Currently we accept and discard status reports.
                tracing::trace!("virtio-input: status queue notified");
                Ok(())
            }
            _ => {
                tracing::warn!("virtio-input: unknown queue index {}", queue_index);
                Ok(())
            }
        }
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_touchscreen_creation() {
        let config = TouchscreenConfig {
            width: 1080,
            height: 1920,
            max_contacts: 10,
        };
        let dev = VirtioInput::new_touchscreen(config);

        assert_eq!(dev.device_type(), VirtioDeviceType::Input);
        assert_eq!(dev.kind(), InputDeviceKind::Touchscreen);
        assert_eq!(dev.name(), "RexPlayer Touchscreen");
    }

    #[test]
    fn test_keyboard_creation() {
        let dev = VirtioInput::new_keyboard();

        assert_eq!(dev.device_type(), VirtioDeviceType::Input);
        assert_eq!(dev.kind(), InputDeviceKind::Keyboard);
        assert_eq!(dev.name(), "RexPlayer Keyboard");
    }

    #[test]
    fn test_event_serialization() {
        let event = InputEvent::new(ev_types::EV_ABS, abs_codes::ABS_MT_POSITION_X, 540);
        let bytes = event.to_bytes();
        let parsed = InputEvent::from_bytes(&bytes).unwrap();

        assert_eq!(parsed.type_, ev_types::EV_ABS);
        assert_eq!(parsed.code, abs_codes::ABS_MT_POSITION_X);
        assert_eq!(parsed.value, 540);
    }

    #[test]
    fn test_event_from_bytes_too_short() {
        let short = [0u8; 4];
        assert!(InputEvent::from_bytes(&short).is_err());
    }

    #[test]
    fn test_syn_report() {
        let syn = InputEvent::syn_report();
        assert_eq!(syn.type_, ev_types::EV_SYN);
        assert_eq!(syn.code, syn_codes::SYN_REPORT);
        assert_eq!(syn.value, 0);
    }

    #[test]
    fn test_inject_touch() {
        let config = TouchscreenConfig::default();
        let mut dev = VirtioInput::new_touchscreen(config);

        // Touch down at (100, 200) with pressure 50
        dev.inject_touch(100, 200, 50);

        // Should have: BTN_TOUCH(1), ABS_X(100), ABS_Y(200), ABS_PRESSURE(50), SYN_REPORT
        assert_eq!(dev.pending_events(), 5);

        let e0 = dev.pop_event().unwrap();
        assert_eq!(e0.type_, ev_types::EV_KEY);
        assert_eq!(e0.code, key_codes::BTN_TOUCH);
        assert_eq!(e0.value, 1);

        let e1 = dev.pop_event().unwrap();
        assert_eq!(e1.type_, ev_types::EV_ABS);
        assert_eq!(e1.code, abs_codes::ABS_X);
        assert_eq!(e1.value, 100);

        let e2 = dev.pop_event().unwrap();
        assert_eq!(e2.type_, ev_types::EV_ABS);
        assert_eq!(e2.code, abs_codes::ABS_Y);
        assert_eq!(e2.value, 200);

        let e3 = dev.pop_event().unwrap();
        assert_eq!(e3.type_, ev_types::EV_ABS);
        assert_eq!(e3.code, abs_codes::ABS_PRESSURE);
        assert_eq!(e3.value, 50);

        let e4 = dev.pop_event().unwrap();
        assert_eq!(e4, InputEvent::syn_report());

        assert_eq!(dev.pending_events(), 0);
    }

    #[test]
    fn test_inject_touch_lift() {
        let config = TouchscreenConfig::default();
        let mut dev = VirtioInput::new_touchscreen(config);

        // Touch lift (pressure = 0)
        dev.inject_touch(0, 0, 0);

        // Should have: BTN_TOUCH(0), ABS_PRESSURE(0), SYN_REPORT
        assert_eq!(dev.pending_events(), 3);

        let e0 = dev.pop_event().unwrap();
        assert_eq!(e0.code, key_codes::BTN_TOUCH);
        assert_eq!(e0.value, 0);

        let e1 = dev.pop_event().unwrap();
        assert_eq!(e1.code, abs_codes::ABS_PRESSURE);
        assert_eq!(e1.value, 0);

        let e2 = dev.pop_event().unwrap();
        assert_eq!(e2, InputEvent::syn_report());
    }

    #[test]
    fn test_inject_multitouch() {
        let config = TouchscreenConfig::default();
        let mut dev = VirtioInput::new_touchscreen(config);

        // Finger 0 down at (100, 200)
        dev.inject_multitouch(0, 0, 100, 200, 50);

        // Should have: MT_SLOT(0), MT_TRACKING_ID(0), MT_POSITION_X(100),
        //              MT_POSITION_Y(200), MT_PRESSURE(50), SYN_REPORT
        assert_eq!(dev.pending_events(), 6);

        let e0 = dev.pop_event().unwrap();
        assert_eq!(e0.code, abs_codes::ABS_MT_SLOT);
        assert_eq!(e0.value, 0);

        let e1 = dev.pop_event().unwrap();
        assert_eq!(e1.code, abs_codes::ABS_MT_TRACKING_ID);
        assert_eq!(e1.value, 0);

        let e2 = dev.pop_event().unwrap();
        assert_eq!(e2.code, abs_codes::ABS_MT_POSITION_X);
        assert_eq!(e2.value, 100);

        let e3 = dev.pop_event().unwrap();
        assert_eq!(e3.code, abs_codes::ABS_MT_POSITION_Y);
        assert_eq!(e3.value, 200);

        let e4 = dev.pop_event().unwrap();
        assert_eq!(e4.code, abs_codes::ABS_MT_PRESSURE);
        assert_eq!(e4.value, 50);

        let e5 = dev.pop_event().unwrap();
        assert_eq!(e5, InputEvent::syn_report());
    }

    #[test]
    fn test_inject_multitouch_lift() {
        let config = TouchscreenConfig::default();
        let mut dev = VirtioInput::new_touchscreen(config);

        // Finger 0 lift (tracking_id = -1)
        dev.inject_multitouch(0, -1, 0, 0, 0);

        // Should have: MT_SLOT(0), MT_TRACKING_ID(-1), SYN_REPORT
        // (no position/pressure for lift)
        assert_eq!(dev.pending_events(), 3);

        let e0 = dev.pop_event().unwrap();
        assert_eq!(e0.code, abs_codes::ABS_MT_SLOT);
        assert_eq!(e0.value, 0);

        let e1 = dev.pop_event().unwrap();
        assert_eq!(e1.code, abs_codes::ABS_MT_TRACKING_ID);
        assert_eq!(e1.value, -1);

        let e2 = dev.pop_event().unwrap();
        assert_eq!(e2, InputEvent::syn_report());
    }

    #[test]
    fn test_inject_key() {
        let mut dev = VirtioInput::new_keyboard();

        // Press Enter
        dev.inject_key(key_codes::KEY_ENTER, 1);
        assert_eq!(dev.pending_events(), 2);

        let e0 = dev.pop_event().unwrap();
        assert_eq!(e0.type_, ev_types::EV_KEY);
        assert_eq!(e0.code, key_codes::KEY_ENTER);
        assert_eq!(e0.value, 1);

        let e1 = dev.pop_event().unwrap();
        assert_eq!(e1, InputEvent::syn_report());

        // Release Enter
        dev.inject_key(key_codes::KEY_ENTER, 0);
        assert_eq!(dev.pending_events(), 2);
    }

    #[test]
    fn test_config_query_name() {
        let dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        let name = dev.query_config(config_select::VIRTIO_INPUT_CFG_ID_NAME, 0);
        assert_eq!(name, b"RexPlayer Touchscreen");
    }

    #[test]
    fn test_config_query_serial() {
        let dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        let serial = dev.query_config(config_select::VIRTIO_INPUT_CFG_ID_SERIAL, 0);
        assert_eq!(serial, b"rex-touch-001");
    }

    #[test]
    fn test_config_query_devids() {
        let dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        let devids = dev.query_config(config_select::VIRTIO_INPUT_CFG_ID_DEVIDS, 0);
        assert_eq!(devids.len(), 8);

        // bustype = 0x06 (BUS_VIRTUAL)
        assert_eq!(u16::from_le_bytes([devids[0], devids[1]]), 0x06);
    }

    #[test]
    fn test_config_query_prop_bits_touchscreen() {
        let dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        let props = dev.query_config(config_select::VIRTIO_INPUT_CFG_PROP_BITS, 0);
        assert!(!props.is_empty());
        assert_eq!(props[0] & 0x01, 0x01); // INPUT_PROP_DIRECT
    }

    #[test]
    fn test_config_query_ev_bits() {
        let dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());

        // Query EV_KEY bits — should include BTN_TOUCH
        let key_bits = dev.query_config(
            config_select::VIRTIO_INPUT_CFG_EV_BITS,
            ev_types::EV_KEY as u8,
        );
        assert!(!key_bits.is_empty());

        // BTN_TOUCH = 0x14a = 330
        let byte_idx = key_codes::BTN_TOUCH as usize / 8;
        let bit_idx = key_codes::BTN_TOUCH as usize % 8;
        assert!(byte_idx < key_bits.len());
        assert!(key_bits[byte_idx] & (1 << bit_idx) != 0);
    }

    #[test]
    fn test_config_query_abs_info() {
        let config = TouchscreenConfig {
            width: 1080,
            height: 1920,
            max_contacts: 10,
        };
        let dev = VirtioInput::new_touchscreen(config);

        // Query ABS_MT_POSITION_X info
        let info = dev.query_config(
            config_select::VIRTIO_INPUT_CFG_ABS_INFO,
            abs_codes::ABS_MT_POSITION_X as u8,
        );
        assert_eq!(info.len(), 20);

        let min = i32::from_le_bytes([info[0], info[1], info[2], info[3]]);
        let max = i32::from_le_bytes([info[4], info[5], info[6], info[7]]);
        assert_eq!(min, 0);
        assert_eq!(max, 1079); // width - 1
    }

    #[test]
    fn test_config_query_abs_info_y() {
        let config = TouchscreenConfig {
            width: 1080,
            height: 1920,
            max_contacts: 10,
        };
        let dev = VirtioInput::new_touchscreen(config);

        let info = dev.query_config(
            config_select::VIRTIO_INPUT_CFG_ABS_INFO,
            abs_codes::ABS_MT_POSITION_Y as u8,
        );
        assert_eq!(info.len(), 20);

        let max = i32::from_le_bytes([info[4], info[5], info[6], info[7]]);
        assert_eq!(max, 1919); // height - 1
    }

    #[test]
    fn test_config_query_unknown() {
        let dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        let result = dev.query_config(0xFF, 0);
        assert!(result.is_empty());
    }

    #[test]
    fn test_keyboard_ev_bits() {
        let dev = VirtioInput::new_keyboard();

        let key_bits = dev.query_config(
            config_select::VIRTIO_INPUT_CFG_EV_BITS,
            ev_types::EV_KEY as u8,
        );
        assert!(!key_bits.is_empty());

        // KEY_ENTER = 28 should be set
        let byte_idx = key_codes::KEY_ENTER as usize / 8;
        let bit_idx = key_codes::KEY_ENTER as usize % 8;
        assert!(byte_idx < key_bits.len());
        assert!(key_bits[byte_idx] & (1 << bit_idx) != 0);

        // KEY_SPACE = 57 should be set
        let byte_idx = key_codes::KEY_SPACE as usize / 8;
        let bit_idx = key_codes::KEY_SPACE as usize % 8;
        assert!(byte_idx < key_bits.len());
        assert!(key_bits[byte_idx] & (1 << bit_idx) != 0);
    }

    #[test]
    fn test_keyboard_no_abs() {
        let dev = VirtioInput::new_keyboard();
        let abs_bits = dev.query_config(
            config_select::VIRTIO_INPUT_CFG_EV_BITS,
            ev_types::EV_ABS as u8,
        );
        assert!(abs_bits.is_empty());
    }

    #[test]
    fn test_activate_and_reset() {
        let mut dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        dev.activate().unwrap();
        assert!(dev.activated);

        dev.inject_touch(100, 200, 50);
        assert!(dev.pending_events() > 0);

        dev.reset();
        assert!(!dev.activated);
        assert_eq!(dev.pending_events(), 0);
    }

    #[test]
    fn test_process_queue() {
        let mut dev = VirtioInput::new_touchscreen(TouchscreenConfig::default());
        dev.activate().unwrap();

        assert!(dev.process_queue(EVENTQ).is_ok());
        assert!(dev.process_queue(STATUSQ).is_ok());
        assert!(dev.process_queue(5).is_ok()); // unknown queue
    }

    #[test]
    fn test_get_abs_info() {
        let config = TouchscreenConfig {
            width: 720,
            height: 1280,
            max_contacts: 5,
        };
        let dev = VirtioInput::new_touchscreen(config);

        let info = dev.get_abs_info(abs_codes::ABS_MT_POSITION_X).unwrap();
        assert_eq!(info.min, 0);
        assert_eq!(info.max, 719);

        let info = dev.get_abs_info(abs_codes::ABS_MT_TRACKING_ID).unwrap();
        assert_eq!(info.max, 4); // max_contacts - 1

        // Non-existent axis
        assert!(dev.get_abs_info(0xFF).is_none());
    }

    #[test]
    fn test_abs_info_serialization() {
        let info = AbsInfo {
            min: -100,
            max: 100,
            fuzz: 4,
            flat: 0,
            res: 10,
        };
        let bytes = info.to_bytes();
        assert_eq!(bytes.len(), 20);

        let min = i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
        let max = i32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
        assert_eq!(min, -100);
        assert_eq!(max, 100);
    }

    #[test]
    fn test_devids_serialization() {
        let ids = InputDevIds {
            bustype: 0x06,
            vendor: 0x1234,
            product: 0x5678,
            version: 0x0001,
        };
        let bytes = ids.to_bytes();
        assert_eq!(bytes.len(), 8);
        assert_eq!(u16::from_le_bytes([bytes[0], bytes[1]]), 0x06);
        assert_eq!(u16::from_le_bytes([bytes[2], bytes[3]]), 0x1234);
        assert_eq!(u16::from_le_bytes([bytes[4], bytes[5]]), 0x5678);
        assert_eq!(u16::from_le_bytes([bytes[6], bytes[7]]), 0x0001);
    }
}
