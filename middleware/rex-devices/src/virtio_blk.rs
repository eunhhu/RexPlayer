//! Virtio block device backend
//!
//! Provides a virtual block device backed by a host file (raw image).
//! Supports read, write, and flush operations.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;

/// Virtio block device feature flags
pub mod features {
    pub const VIRTIO_BLK_F_SIZE_MAX: u64    = 1 << 1;
    pub const VIRTIO_BLK_F_SEG_MAX: u64     = 1 << 2;
    pub const VIRTIO_BLK_F_FLUSH: u64       = 1 << 9;
    pub const VIRTIO_BLK_F_BLK_SIZE: u64    = 1 << 6;
    pub const VIRTIO_BLK_F_RO: u64          = 1 << 5;
}

/// Block request types
#[repr(u32)]
#[derive(Debug, Clone, Copy)]
pub enum BlockRequestType {
    In      = 0,  // Read
    Out     = 1,  // Write
    Flush   = 4,
    GetId   = 8,
}

/// Virtio block device configuration
pub struct VirtioBlkConfig {
    pub image_path: PathBuf,
    pub readonly: bool,
    pub block_size: u32,
}

/// Virtio block device
pub struct VirtioBlk {
    config: VirtioBlkConfig,
    file: Option<File>,
    capacity: u64, // in 512-byte sectors
    features: u64,
    activated: bool,
}

impl VirtioBlk {
    pub fn new(config: VirtioBlkConfig) -> DeviceResult<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(!config.readonly)
            .open(&config.image_path)?;

        let metadata = file.metadata()?;
        let capacity = metadata.len() / 512;

        let mut feat = features::VIRTIO_BLK_F_FLUSH | features::VIRTIO_BLK_F_BLK_SIZE;
        if config.readonly {
            feat |= features::VIRTIO_BLK_F_RO;
        }

        Ok(Self {
            config,
            file: Some(file),
            capacity,
            features: feat,
            activated: false,
        })
    }

    /// Get disk capacity in 512-byte sectors
    pub fn capacity(&self) -> u64 {
        self.capacity
    }

    /// Process a single block request
    pub fn process_request(
        &mut self,
        req_type: u32,
        sector: u64,
        data: &mut [u8],
    ) -> DeviceResult<u8> {
        let file = self.file.as_mut().ok_or(DeviceError::NotReady)?;

        match req_type {
            0 => {
                // Read
                file.seek(SeekFrom::Start(sector * 512))?;
                file.read_exact(data)?;
                Ok(0) // VIRTIO_BLK_S_OK
            }
            1 => {
                // Write
                if self.config.readonly {
                    return Ok(2); // VIRTIO_BLK_S_IOERR
                }
                file.seek(SeekFrom::Start(sector * 512))?;
                file.write_all(data)?;
                Ok(0)
            }
            4 => {
                // Flush
                file.sync_all()?;
                Ok(0)
            }
            _ => Ok(2), // VIRTIO_BLK_S_IOERR (unsupported)
        }
    }
}

impl VirtioDevice for VirtioBlk {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Block
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!(
            "virtio-blk activated: {} ({} sectors, {})",
            self.config.image_path.display(),
            self.capacity,
            if self.config.readonly { "ro" } else { "rw" }
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
    }

    fn process_queue(&mut self, _queue_index: u16) -> DeviceResult<()> {
        // Queue processing requires virtqueue integration
        // Will be connected via the virtio-mmio transport layer
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write as IoWrite;

    fn create_test_image(size: usize) -> (tempfile::NamedTempFile, PathBuf) {
        let mut tmp = tempfile::NamedTempFile::new().unwrap();
        let data = vec![0xABu8; size];
        tmp.write_all(&data).unwrap();
        tmp.flush().unwrap();
        let path = tmp.path().to_path_buf();
        (tmp, path)
    }

    #[test]
    fn test_create_virtio_blk() {
        let (_tmp, path) = create_test_image(4096);
        let blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: false,
            block_size: 512,
        })
        .unwrap();

        assert_eq!(blk.capacity(), 8); // 4096 / 512
        assert_eq!(blk.device_type(), VirtioDeviceType::Block);
    }

    #[test]
    fn test_read_write() {
        let (_tmp, path) = create_test_image(1024);
        let mut blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: false,
            block_size: 512,
        })
        .unwrap();

        // Write to sector 0
        let write_data: Vec<u8> = (0..512).map(|i| (i & 0xFF) as u8).collect();
        let status = blk.process_request(1, 0, &mut write_data.clone()).unwrap();
        assert_eq!(status, 0);

        // Read back sector 0
        let mut read_buf = vec![0u8; 512];
        let status = blk.process_request(0, 0, &mut read_buf).unwrap();
        assert_eq!(status, 0);
        assert_eq!(read_buf, write_data);
    }

    #[test]
    fn test_readonly_rejects_write() {
        let (_tmp, path) = create_test_image(1024);
        let mut blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: true,
            block_size: 512,
        })
        .unwrap();

        let mut data = vec![0u8; 512];
        let status = blk.process_request(1, 0, &mut data).unwrap();
        assert_eq!(status, 2); // VIRTIO_BLK_S_IOERR
    }

    #[test]
    fn test_readonly_features() {
        let (_tmp, path) = create_test_image(1024);
        let blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: true,
            block_size: 512,
        })
        .unwrap();

        assert!(blk.features() & features::VIRTIO_BLK_F_RO != 0);
    }

    #[test]
    fn test_flush() {
        let (_tmp, path) = create_test_image(1024);
        let mut blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: false,
            block_size: 512,
        })
        .unwrap();

        let mut empty = vec![];
        let status = blk.process_request(4, 0, &mut empty).unwrap();
        assert_eq!(status, 0);
    }

    #[test]
    fn test_activate_and_reset() {
        let (_tmp, path) = create_test_image(1024);
        let mut blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: false,
            block_size: 512,
        })
        .unwrap();

        blk.activate().unwrap();
        blk.reset();
    }

    #[test]
    fn test_unsupported_request_type() {
        let (_tmp, path) = create_test_image(1024);
        let mut blk = VirtioBlk::new(VirtioBlkConfig {
            image_path: path,
            readonly: false,
            block_size: 512,
        })
        .unwrap();

        let mut data = vec![0u8; 512];
        let status = blk.process_request(99, 0, &mut data).unwrap();
        assert_eq!(status, 2); // VIRTIO_BLK_S_IOERR
    }
}
