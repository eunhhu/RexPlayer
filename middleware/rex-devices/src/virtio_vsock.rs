//! Virtio vsock device backend
//!
//! Provides AF_VSOCK host-guest communication (used for ADB and Frida).

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};

pub struct VirtioVsock {
    guest_cid: u64,
    features: u64,
    activated: bool,
}

impl VirtioVsock {
    pub fn new(guest_cid: u64) -> Self {
        Self {
            guest_cid,
            features: 0,
            activated: false,
        }
    }

    pub fn guest_cid(&self) -> u64 { self.guest_cid }
}

impl VirtioDevice for VirtioVsock {
    fn device_type(&self) -> VirtioDeviceType { VirtioDeviceType::Vsock }
    fn features(&self) -> u64 { self.features }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!("virtio-vsock activated: CID {}", self.guest_cid);
        Ok(())
    }

    fn reset(&mut self) { self.activated = false; }
    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> { Ok(()) }
}
