//! Virtio network device backend
//!
//! Provides a virtual NIC with TAP/TUN or userspace NAT backend.

use crate::{DeviceResult, VirtioDevice, VirtioDeviceType};

pub struct VirtioNet {
    mac: [u8; 6],
    features: u64,
    activated: bool,
}

impl VirtioNet {
    pub fn new(mac: [u8; 6]) -> Self {
        Self {
            mac,
            features: 0,
            activated: false,
        }
    }

    pub fn mac(&self) -> &[u8; 6] {
        &self.mac
    }
}

impl VirtioDevice for VirtioNet {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Net
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!(
            "virtio-net activated: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            self.mac[0], self.mac[1], self.mac[2],
            self.mac[3], self.mac[4], self.mac[5]
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
    }

    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> {
        Ok(())
    }
}
