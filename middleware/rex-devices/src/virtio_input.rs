//! Virtio input device backend
//!
//! Emulates touchscreen, keyboard, and gamepad via virtio-input protocol.

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::VecDeque;

/// Input event (matches Linux input_event structure)
#[derive(Debug, Clone, Copy)]
pub struct InputEvent {
    pub type_: u16,
    pub code: u16,
    pub value: i32,
}

/// Input device subtype
#[derive(Debug, Clone, Copy)]
pub enum InputDeviceKind {
    Touchscreen,
    Keyboard,
    Gamepad,
}

pub struct VirtioInput {
    kind: InputDeviceKind,
    event_queue: VecDeque<InputEvent>,
    features: u64,
    activated: bool,
}

impl VirtioInput {
    pub fn new(kind: InputDeviceKind) -> Self {
        Self {
            kind,
            event_queue: VecDeque::with_capacity(256),
            features: 0,
            activated: false,
        }
    }

    /// Queue an input event to be delivered to the guest
    pub fn inject_event(&mut self, event: InputEvent) {
        self.event_queue.push_back(event);
    }
}

impl VirtioDevice for VirtioInput {
    fn device_type(&self) -> VirtioDeviceType { VirtioDeviceType::Input }
    fn features(&self) -> u64 { self.features }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!("virtio-input activated: {:?}", self.kind);
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.event_queue.clear();
    }

    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> { Ok(()) }
}
