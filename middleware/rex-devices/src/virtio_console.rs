//! Virtio console device backend
//!
//! Provides a virtual serial console for guest ↔ host communication.

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::VecDeque;

pub struct VirtioConsole {
    rx_buf: VecDeque<u8>,
    features: u64,
    activated: bool,
}

impl VirtioConsole {
    pub fn new() -> Self {
        Self {
            rx_buf: VecDeque::with_capacity(4096),
            features: 0,
            activated: false,
        }
    }

    /// Queue data to be read by the guest
    pub fn inject_input(&mut self, data: &[u8]) {
        self.rx_buf.extend(data);
    }
}

impl VirtioDevice for VirtioConsole {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Console
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!("virtio-console activated");
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.rx_buf.clear();
    }

    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> {
        Ok(())
    }
}
