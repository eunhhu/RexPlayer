//! Virtio GPU device backend (stub)
//!
//! Will integrate with virglrenderer (OpenGL) and Venus (Vulkan) in Phase 3.

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};

pub struct VirtioGpu {
    features: u64,
    activated: bool,
    width: u32,
    height: u32,
}

impl VirtioGpu {
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            features: 0,
            activated: false,
            width,
            height,
        }
    }
}

impl VirtioDevice for VirtioGpu {
    fn device_type(&self) -> VirtioDeviceType { VirtioDeviceType::Gpu }
    fn features(&self) -> u64 { self.features }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!("virtio-gpu activated: {}x{}", self.width, self.height);
        Ok(())
    }

    fn reset(&mut self) { self.activated = false; }
    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> { Ok(()) }
}
