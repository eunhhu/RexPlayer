//! Virtio balloon device backend
//!
//! Dynamic memory management — inflate/deflate guest RAM on demand.

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};

pub struct VirtioBalloon {
    num_pages: u32,
    actual_pages: u32,
    features: u64,
    activated: bool,
}

impl VirtioBalloon {
    pub fn new() -> Self {
        Self {
            num_pages: 0,
            actual_pages: 0,
            features: 0,
            activated: false,
        }
    }

    /// Request the guest to return `pages` 4KB pages
    pub fn inflate(&mut self, pages: u32) {
        self.num_pages = self.num_pages.saturating_add(pages);
    }

    /// Allow the guest to reclaim `pages` 4KB pages
    pub fn deflate(&mut self, pages: u32) {
        self.num_pages = self.num_pages.saturating_sub(pages);
    }
}

impl VirtioDevice for VirtioBalloon {
    fn device_type(&self) -> VirtioDeviceType { VirtioDeviceType::Balloon }
    fn features(&self) -> u64 { self.features }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!("virtio-balloon activated");
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.num_pages = 0;
        self.actual_pages = 0;
    }

    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> { Ok(()) }
}
