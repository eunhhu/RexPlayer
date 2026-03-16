//! Virtio MMIO transport layer
//!
//! Implements the virtio-mmio transport as defined in the virtio specification
//! (Section 4.2). This module handles register reads/writes, device status
//! transitions, feature negotiation, and queue configuration.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};

// ============================================================================
// MMIO register offsets (virtio spec 4.2.2)
// ============================================================================

/// Magic value — reads as "virt" (0x74726976)
pub const MMIO_MAGIC_VALUE: u64 = 0x000;
/// Device version (2 for virtio 1.0+)
pub const MMIO_VERSION: u64 = 0x004;
/// Virtio subsystem device ID
pub const MMIO_DEVICE_ID: u64 = 0x008;
/// Virtio subsystem vendor ID
pub const MMIO_VENDOR_ID: u64 = 0x00C;
/// Flags representing features the device supports (read)
pub const MMIO_DEVICE_FEATURES: u64 = 0x010;
/// Device (host) feature word selection
pub const MMIO_DEVICE_FEATURES_SEL: u64 = 0x014;
/// Flags representing features understood and activated by the driver (write)
pub const MMIO_DRIVER_FEATURES: u64 = 0x020;
/// Activated (driver) feature word selection
pub const MMIO_DRIVER_FEATURES_SEL: u64 = 0x024;
/// Queue selector
pub const MMIO_QUEUE_SEL: u64 = 0x030;
/// Maximum size of the currently selected queue
pub const MMIO_QUEUE_NUM_MAX: u64 = 0x034;
/// Queue size (number of elements)
pub const MMIO_QUEUE_NUM: u64 = 0x038;
/// Ready bit for the currently selected queue
pub const MMIO_QUEUE_READY: u64 = 0x044;
/// Queue notifier (write-only)
pub const MMIO_QUEUE_NOTIFY: u64 = 0x050;
/// Interrupt status (read-only)
pub const MMIO_INTERRUPT_STATUS: u64 = 0x060;
/// Interrupt acknowledge (write-only)
pub const MMIO_INTERRUPT_ACK: u64 = 0x064;
/// Device status
pub const MMIO_STATUS: u64 = 0x070;
/// Descriptor table address (low 32 bits)
pub const MMIO_QUEUE_DESC_LOW: u64 = 0x080;
/// Descriptor table address (high 32 bits)
pub const MMIO_QUEUE_DESC_HIGH: u64 = 0x084;
/// Available ring address (low 32 bits)
pub const MMIO_QUEUE_AVAIL_LOW: u64 = 0x090;
/// Available ring address (high 32 bits)
pub const MMIO_QUEUE_AVAIL_HIGH: u64 = 0x094;
/// Used ring address (low 32 bits)
pub const MMIO_QUEUE_USED_LOW: u64 = 0x0A0;
/// Used ring address (high 32 bits)
pub const MMIO_QUEUE_USED_HIGH: u64 = 0x0A4;
/// Configuration atomicity value
pub const MMIO_CONFIG_GENERATION: u64 = 0x0FC;
/// Start of device-specific configuration space
pub const MMIO_CONFIG_START: u64 = 0x100;

/// Magic value that identifies a virtio-mmio device ("virt" in little-endian)
pub const VIRTIO_MMIO_MAGIC: u32 = 0x7472_6976;
/// Transport version: virtio 1.0 (modern/non-legacy)
pub const VIRTIO_MMIO_VERSION: u32 = 2;
/// RexPlayer vendor ID
pub const VIRTIO_VENDOR_ID: u32 = 0x5245_5800; // "REX\0"

// ============================================================================
// Device status bits (virtio spec 2.1)
// ============================================================================

/// Device status: driver has acknowledged the device
pub const STATUS_ACKNOWLEDGE: u32 = 1;
/// Device status: driver knows how to drive the device
pub const STATUS_DRIVER: u32 = 2;
/// Device status: feature negotiation is complete
pub const STATUS_FEATURES_OK: u32 = 8;
/// Device status: driver is set up and ready
pub const STATUS_DRIVER_OK: u32 = 4;
/// Device status: something went wrong
pub const STATUS_FAILED: u32 = 128;

/// Maximum number of virtqueues per device
const MAX_QUEUES: usize = 8;
/// Default maximum queue size
const DEFAULT_QUEUE_NUM_MAX: u16 = 256;

// ============================================================================
// Per-queue state
// ============================================================================

/// Configuration for a single virtqueue, tracked by the transport
#[derive(Debug, Clone)]
pub struct QueueConfig {
    /// Maximum size supported by the device
    pub num_max: u16,
    /// Size selected by the driver
    pub num: u16,
    /// Whether the queue has been marked ready
    pub ready: bool,
    /// Guest physical address of the descriptor table
    pub desc_addr: u64,
    /// Guest physical address of the available ring
    pub avail_addr: u64,
    /// Guest physical address of the used ring
    pub used_addr: u64,
}

impl Default for QueueConfig {
    fn default() -> Self {
        Self {
            num_max: DEFAULT_QUEUE_NUM_MAX,
            num: 0,
            ready: false,
            desc_addr: 0,
            avail_addr: 0,
            used_addr: 0,
        }
    }
}

// ============================================================================
// VirtioMmioDevice
// ============================================================================

/// Wraps a `VirtioDevice` trait object and exposes it through the virtio-mmio
/// register interface.
pub struct VirtioMmioDevice {
    /// The underlying virtio device backend
    device: Box<dyn VirtioDevice>,

    /// Current device status (bitfield)
    status: u32,

    /// Selected device feature page (0 = low 32 bits, 1 = high 32 bits)
    device_features_sel: u32,
    /// Selected driver feature page
    driver_features_sel: u32,
    /// Features accepted by the driver (full 64-bit)
    driver_features: u64,

    /// Currently selected queue index
    queue_sel: u32,
    /// Per-queue configuration
    queues: Vec<QueueConfig>,

    /// Pending interrupt status bits
    interrupt_status: u32,

    /// Configuration space generation counter (bumped on config changes)
    config_generation: u32,
}

impl VirtioMmioDevice {
    /// Create a new MMIO transport wrapper around a virtio device backend.
    pub fn new(device: Box<dyn VirtioDevice>) -> Self {
        let queues = (0..MAX_QUEUES).map(|_| QueueConfig::default()).collect();

        Self {
            device,
            status: 0,
            device_features_sel: 0,
            driver_features_sel: 0,
            driver_features: 0,
            queue_sel: 0,
            queues,
            interrupt_status: 0,
            config_generation: 0,
        }
    }

    /// Get the device type ID
    pub fn device_type(&self) -> VirtioDeviceType {
        self.device.device_type()
    }

    /// Get the current device status
    pub fn status(&self) -> u32 {
        self.status
    }

    /// Get a reference to a queue config by index, if valid
    fn selected_queue(&self) -> Option<&QueueConfig> {
        self.queues.get(self.queue_sel as usize)
    }

    /// Get a mutable reference to the selected queue config
    fn selected_queue_mut(&mut self) -> Option<&mut QueueConfig> {
        self.queues.get_mut(self.queue_sel as usize)
    }

    /// Check whether the device is in the DRIVER_OK state
    pub fn is_activated(&self) -> bool {
        self.status & STATUS_DRIVER_OK != 0
    }

    /// Get the queue configuration for a given index
    pub fn queue_config(&self, index: u16) -> Option<&QueueConfig> {
        self.queues.get(index as usize)
    }

    /// Set the interrupt status bit for "used buffer notification"
    pub fn raise_interrupt(&mut self) {
        self.interrupt_status |= 0x1;
    }

    /// Set the interrupt status bit for "configuration change notification"
    pub fn raise_config_interrupt(&mut self) {
        self.interrupt_status |= 0x2;
        self.config_generation = self.config_generation.wrapping_add(1);
    }

    // -----------------------------------------------------------------------
    // MMIO read
    // -----------------------------------------------------------------------

    /// Handle a 32-bit MMIO read at the given offset within the device's
    /// register space. Returns the value to be returned to the guest.
    pub fn read(&self, offset: u64) -> u32 {
        match offset {
            MMIO_MAGIC_VALUE => VIRTIO_MMIO_MAGIC,
            MMIO_VERSION => VIRTIO_MMIO_VERSION,
            MMIO_DEVICE_ID => self.device.device_type() as u32,
            MMIO_VENDOR_ID => VIRTIO_VENDOR_ID,

            MMIO_DEVICE_FEATURES => {
                let features = self.device.features();
                match self.device_features_sel {
                    0 => features as u32,
                    1 => (features >> 32) as u32,
                    _ => 0,
                }
            }

            MMIO_QUEUE_NUM_MAX => {
                self.selected_queue()
                    .map(|q| q.num_max as u32)
                    .unwrap_or(0)
            }

            MMIO_QUEUE_READY => {
                self.selected_queue()
                    .map(|q| u32::from(q.ready))
                    .unwrap_or(0)
            }

            MMIO_INTERRUPT_STATUS => self.interrupt_status,

            MMIO_STATUS => self.status,

            MMIO_CONFIG_GENERATION => self.config_generation,

            // Device-specific config reads (offset >= 0x100)
            offset if offset >= MMIO_CONFIG_START => {
                // Config space access is device-specific; return 0 for now.
                // Individual devices can override via a config-read callback.
                0
            }

            _ => {
                tracing::warn!("virtio-mmio: unhandled read at offset {:#x}", offset);
                0
            }
        }
    }

    // -----------------------------------------------------------------------
    // MMIO write
    // -----------------------------------------------------------------------

    /// Handle a 32-bit MMIO write at the given offset within the device's
    /// register space.
    pub fn write(&mut self, offset: u64, value: u32) -> DeviceResult<()> {
        match offset {
            MMIO_DEVICE_FEATURES_SEL => {
                self.device_features_sel = value;
            }

            MMIO_DRIVER_FEATURES_SEL => {
                self.driver_features_sel = value;
            }

            MMIO_DRIVER_FEATURES => {
                // The driver writes one 32-bit page at a time
                match self.driver_features_sel {
                    0 => {
                        self.driver_features =
                            (self.driver_features & 0xFFFF_FFFF_0000_0000)
                                | u64::from(value);
                    }
                    1 => {
                        self.driver_features =
                            (self.driver_features & 0x0000_0000_FFFF_FFFF)
                                | (u64::from(value) << 32);
                    }
                    _ => {}
                }
            }

            MMIO_QUEUE_SEL => {
                if (value as usize) < MAX_QUEUES {
                    self.queue_sel = value;
                }
            }

            MMIO_QUEUE_NUM => {
                if let Some(queue) = self.selected_queue_mut() {
                    let requested = value as u16;
                    // Clamp to the maximum supported by the device
                    if requested <= queue.num_max {
                        queue.num = requested;
                    }
                }
            }

            MMIO_QUEUE_READY => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.ready = value != 0;
                }
            }

            MMIO_QUEUE_DESC_LOW => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.desc_addr =
                        (queue.desc_addr & 0xFFFF_FFFF_0000_0000) | u64::from(value);
                }
            }
            MMIO_QUEUE_DESC_HIGH => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.desc_addr =
                        (queue.desc_addr & 0x0000_0000_FFFF_FFFF)
                            | (u64::from(value) << 32);
                }
            }

            MMIO_QUEUE_AVAIL_LOW => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.avail_addr =
                        (queue.avail_addr & 0xFFFF_FFFF_0000_0000) | u64::from(value);
                }
            }
            MMIO_QUEUE_AVAIL_HIGH => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.avail_addr =
                        (queue.avail_addr & 0x0000_0000_FFFF_FFFF)
                            | (u64::from(value) << 32);
                }
            }

            MMIO_QUEUE_USED_LOW => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.used_addr =
                        (queue.used_addr & 0xFFFF_FFFF_0000_0000) | u64::from(value);
                }
            }
            MMIO_QUEUE_USED_HIGH => {
                if let Some(queue) = self.selected_queue_mut() {
                    queue.used_addr =
                        (queue.used_addr & 0x0000_0000_FFFF_FFFF)
                            | (u64::from(value) << 32);
                }
            }

            MMIO_QUEUE_NOTIFY => {
                let queue_index = value as u16;
                if self.is_activated() {
                    self.device.process_queue(queue_index)?;
                }
            }

            MMIO_INTERRUPT_ACK => {
                // Clear the acknowledged interrupt bits
                self.interrupt_status &= !value;
            }

            MMIO_STATUS => {
                self.handle_status_write(value)?;
            }

            offset if offset >= MMIO_CONFIG_START => {
                // Device-specific config write; currently a no-op.
                // Individual devices can override via a config-write callback.
            }

            _ => {
                tracing::warn!(
                    "virtio-mmio: unhandled write at offset {:#x}, value {:#x}",
                    offset,
                    value
                );
            }
        }

        Ok(())
    }

    // -----------------------------------------------------------------------
    // Status state machine
    // -----------------------------------------------------------------------

    /// Process a write to the STATUS register, enforcing the virtio state
    /// machine:
    ///
    /// ```text
    /// RESET(0) -> ACKNOWLEDGE(1) -> DRIVER(3) -> FEATURES_OK(11)
    ///          -> DRIVER_OK(15)
    /// ```
    ///
    /// Writing 0 resets the device at any point.
    fn handle_status_write(&mut self, value: u32) -> DeviceResult<()> {
        // Writing 0 triggers a full device reset
        if value == 0 {
            self.reset();
            return Ok(());
        }

        let prev = self.status;

        // ACKNOWLEDGE: guest has found and recognised the device
        if value & STATUS_ACKNOWLEDGE != 0 && prev & STATUS_ACKNOWLEDGE == 0 {
            // Valid first step
        }

        // DRIVER: guest knows how to drive the device
        if value & STATUS_DRIVER != 0
            && prev & STATUS_DRIVER == 0
            && prev & STATUS_ACKNOWLEDGE == 0
        {
            // Must acknowledge first
            self.status |= STATUS_FAILED;
            return Err(DeviceError::NotReady);
        }

        // FEATURES_OK: feature negotiation is complete
        if value & STATUS_FEATURES_OK != 0 && prev & STATUS_FEATURES_OK == 0 {
            if prev & STATUS_DRIVER == 0 {
                self.status |= STATUS_FAILED;
                return Err(DeviceError::NotReady);
            }
            // Validate that the driver didn't request unsupported features
            let device_features = self.device.features();
            if self.driver_features & !device_features != 0 {
                // Driver requested features the device doesn't support.
                // Don't set FEATURES_OK — the driver should detect this.
                tracing::warn!(
                    "virtio-mmio: driver requested unsupported features: {:#x}",
                    self.driver_features & !device_features
                );
                // Still update status bits except FEATURES_OK
                self.status = value & !STATUS_FEATURES_OK;
                return Ok(());
            }
        }

        // DRIVER_OK: driver is set up and ready to drive the device
        if value & STATUS_DRIVER_OK != 0 && prev & STATUS_DRIVER_OK == 0 {
            if prev & STATUS_FEATURES_OK == 0 {
                self.status |= STATUS_FAILED;
                return Err(DeviceError::NotReady);
            }
            // Activate the device backend
            self.device.activate()?;
        }

        self.status = value;
        Ok(())
    }

    /// Reset the device to initial state
    fn reset(&mut self) {
        self.status = 0;
        self.driver_features = 0;
        self.device_features_sel = 0;
        self.driver_features_sel = 0;
        self.queue_sel = 0;
        self.interrupt_status = 0;

        for queue in &mut self.queues {
            *queue = QueueConfig::default();
        }

        self.device.reset();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal mock VirtioDevice for testing the transport layer
    struct MockDevice {
        features: u64,
        activated: bool,
        reset_count: u32,
        last_notified_queue: Option<u16>,
    }

    impl MockDevice {
        fn new(features: u64) -> Self {
            Self {
                features,
                activated: false,
                reset_count: 0,
                last_notified_queue: None,
            }
        }
    }

    impl VirtioDevice for MockDevice {
        fn device_type(&self) -> VirtioDeviceType {
            VirtioDeviceType::Console
        }

        fn features(&self) -> u64 {
            self.features
        }

        fn activate(&mut self) -> DeviceResult<()> {
            self.activated = true;
            Ok(())
        }

        fn reset(&mut self) {
            self.activated = false;
            self.reset_count += 1;
        }

        fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
            self.last_notified_queue = Some(queue_index);
            Ok(())
        }
    }

    fn make_mmio() -> VirtioMmioDevice {
        VirtioMmioDevice::new(Box::new(MockDevice::new(0x0000_0001_0000_003F)))
    }

    #[test]
    fn magic_and_version() {
        let dev = make_mmio();
        assert_eq!(dev.read(MMIO_MAGIC_VALUE), VIRTIO_MMIO_MAGIC);
        assert_eq!(dev.read(MMIO_VERSION), VIRTIO_MMIO_VERSION);
    }

    #[test]
    fn device_id_and_vendor() {
        let dev = make_mmio();
        assert_eq!(dev.read(MMIO_DEVICE_ID), VirtioDeviceType::Console as u32);
        assert_eq!(dev.read(MMIO_VENDOR_ID), VIRTIO_VENDOR_ID);
    }

    #[test]
    fn device_features_selection() {
        let mut dev = make_mmio();

        // Select low 32 bits (page 0)
        dev.write(MMIO_DEVICE_FEATURES_SEL, 0).unwrap();
        assert_eq!(dev.read(MMIO_DEVICE_FEATURES), 0x0000_003F);

        // Select high 32 bits (page 1)
        dev.write(MMIO_DEVICE_FEATURES_SEL, 1).unwrap();
        assert_eq!(dev.read(MMIO_DEVICE_FEATURES), 0x0000_0001);
    }

    #[test]
    fn driver_features_negotiation() {
        let mut dev = make_mmio();

        // Write driver features low
        dev.write(MMIO_DRIVER_FEATURES_SEL, 0).unwrap();
        dev.write(MMIO_DRIVER_FEATURES, 0x0000_000F).unwrap();

        // Write driver features high
        dev.write(MMIO_DRIVER_FEATURES_SEL, 1).unwrap();
        dev.write(MMIO_DRIVER_FEATURES, 0x0000_0001).unwrap();

        assert_eq!(dev.driver_features, 0x0000_0001_0000_000F);
    }

    #[test]
    fn status_state_machine_happy_path() {
        let mut dev = make_mmio();

        // Accept only features the device supports
        dev.write(MMIO_DRIVER_FEATURES_SEL, 0).unwrap();
        dev.write(MMIO_DRIVER_FEATURES, 0x0000_0001).unwrap();
        dev.write(MMIO_DRIVER_FEATURES_SEL, 1).unwrap();
        dev.write(MMIO_DRIVER_FEATURES, 0x0000_0000).unwrap();

        // RESET -> ACKNOWLEDGE
        dev.write(MMIO_STATUS, STATUS_ACKNOWLEDGE).unwrap();
        assert_eq!(dev.status(), STATUS_ACKNOWLEDGE);

        // ACKNOWLEDGE -> DRIVER
        dev.write(MMIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER)
            .unwrap();
        assert_eq!(dev.status(), STATUS_ACKNOWLEDGE | STATUS_DRIVER);

        // DRIVER -> FEATURES_OK
        dev.write(
            MMIO_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK,
        )
        .unwrap();
        assert_eq!(
            dev.status(),
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK
        );

        // FEATURES_OK -> DRIVER_OK
        dev.write(
            MMIO_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
        )
        .unwrap();
        assert!(dev.is_activated());
    }

    #[test]
    fn status_reset() {
        let mut dev = make_mmio();

        dev.write(MMIO_STATUS, STATUS_ACKNOWLEDGE).unwrap();
        assert_eq!(dev.status(), STATUS_ACKNOWLEDGE);

        // Writing 0 resets
        dev.write(MMIO_STATUS, 0).unwrap();
        assert_eq!(dev.status(), 0);
    }

    #[test]
    fn queue_selection_and_config() {
        let mut dev = make_mmio();

        // Select queue 0
        dev.write(MMIO_QUEUE_SEL, 0).unwrap();
        assert_eq!(dev.read(MMIO_QUEUE_NUM_MAX), DEFAULT_QUEUE_NUM_MAX as u32);

        // Set queue size
        dev.write(MMIO_QUEUE_NUM, 128).unwrap();
        assert_eq!(dev.queue_config(0).unwrap().num, 128);

        // Set descriptor table address
        dev.write(MMIO_QUEUE_DESC_LOW, 0xDEAD_0000).unwrap();
        dev.write(MMIO_QUEUE_DESC_HIGH, 0x0000_0001).unwrap();
        assert_eq!(
            dev.queue_config(0).unwrap().desc_addr,
            0x0000_0001_DEAD_0000
        );

        // Set available ring address
        dev.write(MMIO_QUEUE_AVAIL_LOW, 0xBEEF_0000).unwrap();
        dev.write(MMIO_QUEUE_AVAIL_HIGH, 0x0000_0002).unwrap();
        assert_eq!(
            dev.queue_config(0).unwrap().avail_addr,
            0x0000_0002_BEEF_0000
        );

        // Set used ring address
        dev.write(MMIO_QUEUE_USED_LOW, 0xCAFE_0000).unwrap();
        dev.write(MMIO_QUEUE_USED_HIGH, 0x0000_0003).unwrap();
        assert_eq!(
            dev.queue_config(0).unwrap().used_addr,
            0x0000_0003_CAFE_0000
        );

        // Mark queue ready
        dev.write(MMIO_QUEUE_READY, 1).unwrap();
        assert!(dev.queue_config(0).unwrap().ready);
    }

    #[test]
    fn interrupt_ack() {
        let mut dev = make_mmio();

        dev.raise_interrupt();
        assert_eq!(dev.read(MMIO_INTERRUPT_STATUS), 0x1);

        dev.raise_config_interrupt();
        assert_eq!(dev.read(MMIO_INTERRUPT_STATUS), 0x3);

        // Acknowledge the used-buffer interrupt
        dev.write(MMIO_INTERRUPT_ACK, 0x1).unwrap();
        assert_eq!(dev.read(MMIO_INTERRUPT_STATUS), 0x2);

        // Acknowledge the config interrupt
        dev.write(MMIO_INTERRUPT_ACK, 0x2).unwrap();
        assert_eq!(dev.read(MMIO_INTERRUPT_STATUS), 0x0);
    }

    #[test]
    fn queue_num_clamped_to_max() {
        let mut dev = make_mmio();

        dev.write(MMIO_QUEUE_SEL, 0).unwrap();

        // Try to set a queue size larger than the max
        dev.write(MMIO_QUEUE_NUM, (DEFAULT_QUEUE_NUM_MAX as u32) + 100)
            .unwrap();
        // Should not have changed (stays at 0, the default)
        assert_eq!(dev.queue_config(0).unwrap().num, 0);

        // Set a valid size
        dev.write(MMIO_QUEUE_NUM, 64).unwrap();
        assert_eq!(dev.queue_config(0).unwrap().num, 64);
    }

    #[test]
    fn config_generation_increments() {
        let mut dev = make_mmio();

        let gen0 = dev.read(MMIO_CONFIG_GENERATION);
        dev.raise_config_interrupt();
        let gen1 = dev.read(MMIO_CONFIG_GENERATION);
        assert_eq!(gen1, gen0 + 1);
    }
}
