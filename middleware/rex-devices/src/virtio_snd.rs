//! Virtio sound device backend (stub)
//!
//! Will integrate with host audio API (PulseAudio/PipeWire/CoreAudio) in Phase 3.

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};

pub struct VirtioSnd {
    features: u64,
    activated: bool,
}

impl VirtioSnd {
    pub fn new() -> Self {
        Self {
            features: 0,
            activated: false,
        }
    }
}

impl VirtioDevice for VirtioSnd {
    fn device_type(&self) -> VirtioDeviceType { VirtioDeviceType::Sound }
    fn features(&self) -> u64 { self.features }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!("virtio-snd activated");
        Ok(())
    }

    fn reset(&mut self) { self.activated = false; }
    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> { Ok(()) }
}
