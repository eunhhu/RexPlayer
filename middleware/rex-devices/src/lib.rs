//! Virtio device backends for RexPlayer
//!
//! Each device implements the virtio protocol over virtio-mmio transport.
//! Uses rust-vmm crates (vm-memory, virtio-queue) for virtqueue management.

pub mod virtio_blk;
pub mod virtio_console;
pub mod virtio_net;
pub mod virtio_gpu;
pub mod virtio_vsock;
pub mod virtio_input;
pub mod virtio_balloon;
pub mod virtio_snd;
pub mod virtio_mmio;
pub mod virtqueue;

use thiserror::Error;

/// Common error type for device operations
#[derive(Error, Debug)]
pub enum DeviceError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Invalid virtqueue descriptor")]
    InvalidDescriptor,

    #[error("Guest memory access failed at GPA {0:#x}")]
    GuestMemory(u64),

    #[error("Device not ready")]
    NotReady,

    #[error("Unsupported feature: {0}")]
    UnsupportedFeature(String),
}

pub type DeviceResult<T> = Result<T, DeviceError>;

/// Virtio device type IDs (from virtio spec)
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VirtioDeviceType {
    Net       = 1,
    Block     = 2,
    Console   = 3,
    Balloon   = 5,
    Gpu       = 16,
    Input     = 18,
    Vsock     = 19,
    Sound     = 25,
}

/// Common trait for all virtio devices
pub trait VirtioDevice: Send {
    /// Device type ID
    fn device_type(&self) -> VirtioDeviceType;

    /// Negotiated features
    fn features(&self) -> u64;

    /// Activate the device (after feature negotiation)
    fn activate(&mut self) -> DeviceResult<()>;

    /// Reset the device
    fn reset(&mut self);

    /// Process available virtqueue buffers
    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()>;
}
