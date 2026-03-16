//! Virtio balloon device backend
//!
//! Implements the virtio-balloon device per virtio specification (Section 5.5).
//! Provides dynamic memory management — inflate/deflate guest RAM on demand.
//! Supports memory statistics reporting and free page hinting.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::BTreeSet;

// ============================================================================
// Feature flags (virtio spec 5.5.3)
// ============================================================================

pub mod features {
    /// Host can handle deflation requests during OOM
    pub const VIRTIO_BALLOON_F_DEFLATE_ON_OOM: u64 = 1 << 2;
    /// Enable memory statistics reporting via stats VQ
    pub const VIRTIO_BALLOON_F_STATS_VQ: u64 = 1 << 1;
    /// Enable free page hinting
    pub const VIRTIO_BALLOON_F_FREE_PAGE_HINT: u64 = 1 << 3;
}

// ============================================================================
// Virtqueue indices
// ============================================================================

/// Inflate queue: guest reports pages to take away
pub const INFLATE_QUEUE: u16 = 0;
/// Deflate queue: guest reports pages to give back
pub const DEFLATE_QUEUE: u16 = 1;
/// Stats queue: guest reports memory statistics (optional, requires STATS_VQ)
pub const STATS_QUEUE: u16 = 2;

// ============================================================================
// Memory statistics tags (virtio spec 5.5.6)
// ============================================================================

pub mod stat_tags {
    /// Amount of memory swapped in (in bytes)
    pub const VIRTIO_BALLOON_S_SWAP_IN: u16 = 0;
    /// Amount of memory swapped out (in bytes)
    pub const VIRTIO_BALLOON_S_SWAP_OUT: u16 = 1;
    /// Number of major page faults
    pub const VIRTIO_BALLOON_S_MAJFLT: u16 = 2;
    /// Number of minor page faults
    pub const VIRTIO_BALLOON_S_MINFLT: u16 = 3;
    /// Amount of free memory (in bytes)
    pub const VIRTIO_BALLOON_S_MEMFREE: u16 = 4;
    /// Total amount of memory (in bytes)
    pub const VIRTIO_BALLOON_S_MEMTOT: u16 = 5;
    /// Amount of available memory (in bytes)
    pub const VIRTIO_BALLOON_S_AVAIL: u16 = 6;
    /// Amount of disk caches (in bytes)
    pub const VIRTIO_BALLOON_S_CACHES: u16 = 7;
    /// Number of hugetlb page allocations
    pub const VIRTIO_BALLOON_S_HTLB_PGALLOC: u16 = 8;
    /// Number of hugetlb page allocation failures
    pub const VIRTIO_BALLOON_S_HTLB_PGFAIL: u16 = 9;

    /// Number of known stat tags
    pub const VIRTIO_BALLOON_S_NR: usize = 10;
}

/// Size of a single stat entry in bytes (tag: u16 + val: u64 = 10 bytes)
pub const STAT_ENTRY_SIZE: usize = 10;

// ============================================================================
// Balloon config space (virtio spec 5.5.4)
// ============================================================================

/// Size of the balloon config space in bytes
pub const CONFIG_SPACE_SIZE: usize = 8;

/// A single memory statistic entry from the guest
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BalloonStat {
    /// Statistic tag (one of VIRTIO_BALLOON_S_*)
    pub tag: u16,
    /// Statistic value
    pub val: u64,
}

impl BalloonStat {
    /// Serialize to bytes (little-endian, 10 bytes)
    pub fn to_bytes(&self) -> [u8; STAT_ENTRY_SIZE] {
        let mut buf = [0u8; STAT_ENTRY_SIZE];
        buf[0..2].copy_from_slice(&self.tag.to_le_bytes());
        buf[2..10].copy_from_slice(&self.val.to_le_bytes());
        buf
    }

    /// Parse from bytes (little-endian)
    pub fn from_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < STAT_ENTRY_SIZE {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            tag: u16::from_le_bytes([buf[0], buf[1]]),
            val: u64::from_le_bytes(buf[2..10].try_into().unwrap()),
        })
    }
}

// ============================================================================
// Balloon state
// ============================================================================

/// Internal state tracking for the balloon device
#[derive(Debug)]
pub struct BalloonState {
    /// Target number of 4KB pages the device wants the balloon to hold
    pub target_pages: u32,
    /// Number of 4KB pages the balloon is currently holding
    pub actual_pages: u32,
    /// Set of page frame numbers (PFNs) currently inflated
    pub inflated_pfns: BTreeSet<u32>,
    /// Last known memory stats from the guest
    pub last_stats: Vec<BalloonStat>,
    /// Whether a stats request is outstanding (waiting for guest response)
    pub stats_request_pending: bool,
}

impl BalloonState {
    pub fn new() -> Self {
        Self {
            target_pages: 0,
            actual_pages: 0,
            inflated_pfns: BTreeSet::new(),
            last_stats: Vec::new(),
            stats_request_pending: false,
        }
    }

    /// Reset all state
    pub fn reset(&mut self) {
        self.target_pages = 0;
        self.actual_pages = 0;
        self.inflated_pfns.clear();
        self.last_stats.clear();
        self.stats_request_pending = false;
    }
}

impl Default for BalloonState {
    fn default() -> Self {
        Self::new()
    }
}

// ============================================================================
// VirtioBalloon device
// ============================================================================

/// Virtio balloon device for dynamic guest memory management.
///
/// The host sets a target balloon size via `set_target()`. The guest driver
/// then inflates or deflates the balloon by reporting page frame numbers
/// through the inflate/deflate virtqueues. Optionally, the guest can
/// report memory statistics through the stats virtqueue.
pub struct VirtioBalloon {
    /// Balloon state
    state: BalloonState,
    /// Negotiated feature flags
    features: u64,
    /// Whether the device has been activated
    activated: bool,
}

impl VirtioBalloon {
    /// Create a new balloon device with default features.
    ///
    /// Enables DEFLATE_ON_OOM and STATS_VQ by default.
    pub fn new() -> Self {
        let features = features::VIRTIO_BALLOON_F_DEFLATE_ON_OOM
            | features::VIRTIO_BALLOON_F_STATS_VQ;

        Self {
            state: BalloonState::new(),
            features,
            activated: false,
        }
    }

    /// Create a new balloon device with specific feature flags.
    pub fn with_features(features: u64) -> Self {
        Self {
            state: BalloonState::new(),
            features,
            activated: false,
        }
    }

    // ========================================================================
    // Config space
    // ========================================================================

    /// Read the config space.
    ///
    /// Layout (8 bytes, little-endian):
    /// - offset 0: num_pages (u32) — target balloon size
    /// - offset 4: actual (u32) — actual balloon size
    pub fn read_config(&self) -> [u8; CONFIG_SPACE_SIZE] {
        let mut buf = [0u8; CONFIG_SPACE_SIZE];
        buf[0..4].copy_from_slice(&self.state.target_pages.to_le_bytes());
        buf[4..8].copy_from_slice(&self.state.actual_pages.to_le_bytes());
        buf
    }

    /// Read a specific offset from the config space.
    pub fn read_config_at(&self, offset: usize) -> DeviceResult<u32> {
        match offset {
            0 => Ok(self.state.target_pages),
            4 => Ok(self.state.actual_pages),
            _ => Err(DeviceError::UnsupportedFeature(format!(
                "invalid balloon config offset: {}",
                offset
            ))),
        }
    }

    /// Write to the config space (guest writes `actual` field).
    pub fn write_config_at(&mut self, offset: usize, value: u32) -> DeviceResult<()> {
        match offset {
            4 => {
                self.state.actual_pages = value;
                tracing::debug!("balloon: guest reports actual_pages = {}", value);
                Ok(())
            }
            _ => Err(DeviceError::UnsupportedFeature(format!(
                "invalid balloon config write offset: {}",
                offset
            ))),
        }
    }

    // ========================================================================
    // Target management (host → guest)
    // ========================================================================

    /// Set the target balloon size in number of 4KB pages.
    ///
    /// The guest will inflate or deflate to reach this target.
    pub fn set_target(&mut self, target_pages: u32) {
        self.state.target_pages = target_pages;
        tracing::info!("balloon: target set to {} pages ({} MB)",
            target_pages,
            (target_pages as u64 * 4096) / (1024 * 1024)
        );
    }

    /// Set the target balloon size in megabytes (convenience method).
    pub fn set_target_mb(&mut self, megabytes: u32) {
        let pages = (megabytes as u64 * 1024 * 1024 / 4096) as u32;
        self.set_target(pages);
    }

    /// Get the current target in pages.
    pub fn target_pages(&self) -> u32 {
        self.state.target_pages
    }

    /// Get the current actual balloon size in pages.
    pub fn actual_pages(&self) -> u32 {
        self.state.actual_pages
    }

    /// Get the number of individually tracked inflated PFNs.
    pub fn inflated_pfn_count(&self) -> usize {
        self.state.inflated_pfns.len()
    }

    // ========================================================================
    // Inflate / Deflate processing
    // ========================================================================

    /// Process a list of page frame numbers from the inflate queue.
    ///
    /// The guest is reporting these pages as "given to the balloon" —
    /// the host can reclaim/free the backing memory for these pages.
    pub fn inflate(&mut self, pfns: &[u32]) {
        for &pfn in pfns {
            self.state.inflated_pfns.insert(pfn);
        }
        self.state.actual_pages = self.state.inflated_pfns.len() as u32;
        tracing::debug!(
            "balloon: inflated {} pages, total now {}",
            pfns.len(),
            self.state.actual_pages
        );
    }

    /// Process a list of page frame numbers from the deflate queue.
    ///
    /// The guest is reclaiming these pages back from the balloon.
    pub fn deflate(&mut self, pfns: &[u32]) {
        for &pfn in pfns {
            self.state.inflated_pfns.remove(&pfn);
        }
        self.state.actual_pages = self.state.inflated_pfns.len() as u32;
        tracing::debug!(
            "balloon: deflated {} pages, total now {}",
            pfns.len(),
            self.state.actual_pages
        );
    }

    /// Parse PFN list from raw bytes (each PFN is a u32, little-endian).
    pub fn parse_pfn_list(data: &[u8]) -> Vec<u32> {
        data.chunks_exact(4)
            .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
            .collect()
    }

    /// Check if a specific PFN is currently inflated.
    pub fn is_pfn_inflated(&self, pfn: u32) -> bool {
        self.state.inflated_pfns.contains(&pfn)
    }

    // ========================================================================
    // Stats processing
    // ========================================================================

    /// Process raw stats data from the stats virtqueue.
    ///
    /// The guest sends a buffer containing stat entries (10 bytes each:
    /// u16 tag + u64 value, both little-endian).
    pub fn update_stats(&mut self, data: &[u8]) -> DeviceResult<()> {
        let mut stats = Vec::new();
        let mut offset = 0;

        while offset + STAT_ENTRY_SIZE <= data.len() {
            let stat = BalloonStat::from_bytes(&data[offset..])?;
            stats.push(stat);
            offset += STAT_ENTRY_SIZE;
        }

        self.state.last_stats = stats;
        self.state.stats_request_pending = false;

        tracing::debug!(
            "balloon: received {} stat entries from guest",
            self.state.last_stats.len()
        );
        Ok(())
    }

    /// Request the guest to send updated memory statistics.
    ///
    /// The host signals this by making a buffer available on the stats queue.
    /// Returns false if a request is already pending.
    pub fn request_stats(&mut self) -> bool {
        if self.state.stats_request_pending {
            return false;
        }
        self.state.stats_request_pending = true;
        true
    }

    /// Get the last known memory statistics from the guest.
    pub fn get_stats(&self) -> &[BalloonStat] {
        &self.state.last_stats
    }

    /// Look up a specific stat by tag.
    pub fn get_stat(&self, tag: u16) -> Option<u64> {
        self.state
            .last_stats
            .iter()
            .find(|s| s.tag == tag)
            .map(|s| s.val)
    }

    /// Check if a stats request is currently pending.
    pub fn stats_request_pending(&self) -> bool {
        self.state.stats_request_pending
    }

    /// Get a reference to the internal balloon state.
    pub fn state(&self) -> &BalloonState {
        &self.state
    }
}

impl Default for VirtioBalloon {
    fn default() -> Self {
        Self::new()
    }
}

impl VirtioDevice for VirtioBalloon {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Balloon
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!(
            "virtio-balloon activated: features={:#x}, target={} pages",
            self.features,
            self.state.target_pages
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.state.reset();
        tracing::debug!("virtio-balloon reset");
    }

    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
        match queue_index {
            INFLATE_QUEUE => {
                tracing::trace!("virtio-balloon: inflate queue notified");
                Ok(())
            }
            DEFLATE_QUEUE => {
                tracing::trace!("virtio-balloon: deflate queue notified");
                Ok(())
            }
            STATS_QUEUE => {
                tracing::trace!("virtio-balloon: stats queue notified");
                Ok(())
            }
            _ => {
                tracing::warn!("virtio-balloon: unknown queue index {}", queue_index);
                Ok(())
            }
        }
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_creation_and_features() {
        let dev = VirtioBalloon::new();
        assert_eq!(dev.device_type(), VirtioDeviceType::Balloon);
        assert!(dev.features() & features::VIRTIO_BALLOON_F_DEFLATE_ON_OOM != 0);
        assert!(dev.features() & features::VIRTIO_BALLOON_F_STATS_VQ != 0);
    }

    #[test]
    fn test_creation_with_custom_features() {
        let dev = VirtioBalloon::with_features(
            features::VIRTIO_BALLOON_F_FREE_PAGE_HINT,
        );
        assert!(dev.features() & features::VIRTIO_BALLOON_F_FREE_PAGE_HINT != 0);
        assert!(dev.features() & features::VIRTIO_BALLOON_F_STATS_VQ == 0);
    }

    #[test]
    fn test_config_space_read() {
        let mut dev = VirtioBalloon::new();
        dev.set_target(256);

        // Read full config space
        let config = dev.read_config();
        let num_pages = u32::from_le_bytes([config[0], config[1], config[2], config[3]]);
        let actual = u32::from_le_bytes([config[4], config[5], config[6], config[7]]);
        assert_eq!(num_pages, 256);
        assert_eq!(actual, 0);

        // Read specific offsets
        assert_eq!(dev.read_config_at(0).unwrap(), 256);
        assert_eq!(dev.read_config_at(4).unwrap(), 0);
        assert!(dev.read_config_at(8).is_err());
    }

    #[test]
    fn test_config_space_write() {
        let mut dev = VirtioBalloon::new();

        // Guest writes actual_pages at offset 4
        dev.write_config_at(4, 100).unwrap();
        assert_eq!(dev.read_config_at(4).unwrap(), 100);

        // Cannot write to offset 0 (num_pages is read-only from guest perspective)
        assert!(dev.write_config_at(0, 50).is_err());
    }

    #[test]
    fn test_inflate_pages() {
        let mut dev = VirtioBalloon::new();
        dev.set_target(10);

        // Inflate with some PFNs
        dev.inflate(&[100, 200, 300, 400, 500]);
        assert_eq!(dev.actual_pages(), 5);
        assert_eq!(dev.inflated_pfn_count(), 5);
        assert!(dev.is_pfn_inflated(100));
        assert!(dev.is_pfn_inflated(300));
        assert!(!dev.is_pfn_inflated(150));

        // Inflate more
        dev.inflate(&[600, 700]);
        assert_eq!(dev.actual_pages(), 7);

        // Duplicate PFNs should not increase count
        dev.inflate(&[100, 200]);
        assert_eq!(dev.actual_pages(), 7);
    }

    #[test]
    fn test_deflate_pages() {
        let mut dev = VirtioBalloon::new();

        // Inflate first
        dev.inflate(&[10, 20, 30, 40, 50]);
        assert_eq!(dev.actual_pages(), 5);

        // Deflate some pages
        dev.deflate(&[20, 40]);
        assert_eq!(dev.actual_pages(), 3);
        assert!(!dev.is_pfn_inflated(20));
        assert!(!dev.is_pfn_inflated(40));
        assert!(dev.is_pfn_inflated(10));
        assert!(dev.is_pfn_inflated(30));
        assert!(dev.is_pfn_inflated(50));

        // Deflate non-existent PFN is a no-op
        dev.deflate(&[999]);
        assert_eq!(dev.actual_pages(), 3);
    }

    #[test]
    fn test_target_size_adjustment() {
        let mut dev = VirtioBalloon::new();
        assert_eq!(dev.target_pages(), 0);

        dev.set_target(1024);
        assert_eq!(dev.target_pages(), 1024);

        // Set target in MB
        dev.set_target_mb(16); // 16 MB = 4096 pages
        assert_eq!(dev.target_pages(), 4096);

        // Verify config space reflects target
        assert_eq!(dev.read_config_at(0).unwrap(), 4096);
    }

    #[test]
    fn test_stats_parsing() {
        let mut dev = VirtioBalloon::new();

        // Build a stats buffer: MEMFREE=1048576, MEMTOT=4194304
        let mut data = Vec::new();
        let stat1 = BalloonStat {
            tag: stat_tags::VIRTIO_BALLOON_S_MEMFREE,
            val: 1_048_576,
        };
        let stat2 = BalloonStat {
            tag: stat_tags::VIRTIO_BALLOON_S_MEMTOT,
            val: 4_194_304,
        };
        data.extend_from_slice(&stat1.to_bytes());
        data.extend_from_slice(&stat2.to_bytes());

        dev.update_stats(&data).unwrap();

        let stats = dev.get_stats();
        assert_eq!(stats.len(), 2);

        assert_eq!(
            dev.get_stat(stat_tags::VIRTIO_BALLOON_S_MEMFREE),
            Some(1_048_576)
        );
        assert_eq!(
            dev.get_stat(stat_tags::VIRTIO_BALLOON_S_MEMTOT),
            Some(4_194_304)
        );
        assert_eq!(
            dev.get_stat(stat_tags::VIRTIO_BALLOON_S_SWAP_IN),
            None
        );
    }

    #[test]
    fn test_stats_request_lifecycle() {
        let mut dev = VirtioBalloon::new();

        // No request pending initially
        assert!(!dev.stats_request_pending());

        // First request succeeds
        assert!(dev.request_stats());
        assert!(dev.stats_request_pending());

        // Second request while pending fails
        assert!(!dev.request_stats());

        // After receiving stats, no longer pending
        let stat = BalloonStat {
            tag: stat_tags::VIRTIO_BALLOON_S_MEMFREE,
            val: 1024,
        };
        dev.update_stats(&stat.to_bytes()).unwrap();
        assert!(!dev.stats_request_pending());

        // Can request again
        assert!(dev.request_stats());
    }

    #[test]
    fn test_activate_reset_lifecycle() {
        let mut dev = VirtioBalloon::new();

        // Activate
        dev.activate().unwrap();

        // Set state
        dev.set_target(512);
        dev.inflate(&[1, 2, 3, 4, 5]);
        let stat = BalloonStat {
            tag: stat_tags::VIRTIO_BALLOON_S_MEMFREE,
            val: 999,
        };
        dev.update_stats(&stat.to_bytes()).unwrap();

        assert_eq!(dev.target_pages(), 512);
        assert_eq!(dev.actual_pages(), 5);
        assert!(!dev.get_stats().is_empty());

        // Reset clears everything
        dev.reset();
        assert_eq!(dev.target_pages(), 0);
        assert_eq!(dev.actual_pages(), 0);
        assert!(dev.get_stats().is_empty());
        assert_eq!(dev.inflated_pfn_count(), 0);
    }

    #[test]
    fn test_feature_negotiation() {
        // Default features
        let dev = VirtioBalloon::new();
        assert!(dev.features() & features::VIRTIO_BALLOON_F_DEFLATE_ON_OOM != 0);
        assert!(dev.features() & features::VIRTIO_BALLOON_F_STATS_VQ != 0);
        assert!(dev.features() & features::VIRTIO_BALLOON_F_FREE_PAGE_HINT == 0);

        // All features
        let dev = VirtioBalloon::with_features(
            features::VIRTIO_BALLOON_F_DEFLATE_ON_OOM
                | features::VIRTIO_BALLOON_F_STATS_VQ
                | features::VIRTIO_BALLOON_F_FREE_PAGE_HINT,
        );
        assert!(dev.features() & features::VIRTIO_BALLOON_F_FREE_PAGE_HINT != 0);

        // No features
        let dev = VirtioBalloon::with_features(0);
        assert_eq!(dev.features(), 0);
    }

    #[test]
    fn test_parse_pfn_list() {
        let pfns: Vec<u32> = vec![100, 200, 300];
        let mut data = Vec::new();
        for pfn in &pfns {
            data.extend_from_slice(&pfn.to_le_bytes());
        }

        let parsed = VirtioBalloon::parse_pfn_list(&data);
        assert_eq!(parsed, pfns);

        // Trailing bytes less than 4 are ignored
        let mut data_with_extra = data.clone();
        data_with_extra.push(0xFF);
        data_with_extra.push(0xAA);
        let parsed = VirtioBalloon::parse_pfn_list(&data_with_extra);
        assert_eq!(parsed, pfns);

        // Empty input
        let parsed = VirtioBalloon::parse_pfn_list(&[]);
        assert!(parsed.is_empty());
    }

    #[test]
    fn test_stat_serialization() {
        let stat = BalloonStat {
            tag: stat_tags::VIRTIO_BALLOON_S_AVAIL,
            val: 0x0102_0304_0506_0708,
        };

        let bytes = stat.to_bytes();
        assert_eq!(bytes.len(), STAT_ENTRY_SIZE);

        let parsed = BalloonStat::from_bytes(&bytes).unwrap();
        assert_eq!(parsed.tag, stat_tags::VIRTIO_BALLOON_S_AVAIL);
        assert_eq!(parsed.val, 0x0102_0304_0506_0708);

        // Too-short buffer
        assert!(BalloonStat::from_bytes(&[0; 5]).is_err());
    }

    #[test]
    fn test_process_queue() {
        let mut dev = VirtioBalloon::new();
        dev.activate().unwrap();

        assert!(dev.process_queue(INFLATE_QUEUE).is_ok());
        assert!(dev.process_queue(DEFLATE_QUEUE).is_ok());
        assert!(dev.process_queue(STATS_QUEUE).is_ok());
        assert!(dev.process_queue(99).is_ok()); // unknown queue
    }

    #[test]
    fn test_default_trait() {
        let dev = VirtioBalloon::default();
        assert_eq!(dev.device_type(), VirtioDeviceType::Balloon);
        assert!(dev.features() & features::VIRTIO_BALLOON_F_DEFLATE_ON_OOM != 0);
    }

    #[test]
    fn test_all_stat_tags() {
        let mut dev = VirtioBalloon::new();

        // Build stats with all known tags
        let all_stats = vec![
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_SWAP_IN, val: 1000 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_SWAP_OUT, val: 2000 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_MAJFLT, val: 50 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_MINFLT, val: 500 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_MEMFREE, val: 1_000_000 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_MEMTOT, val: 4_000_000 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_AVAIL, val: 2_000_000 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_CACHES, val: 500_000 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_HTLB_PGALLOC, val: 10 },
            BalloonStat { tag: stat_tags::VIRTIO_BALLOON_S_HTLB_PGFAIL, val: 1 },
        ];

        let mut data = Vec::new();
        for stat in &all_stats {
            data.extend_from_slice(&stat.to_bytes());
        }

        dev.update_stats(&data).unwrap();
        assert_eq!(dev.get_stats().len(), stat_tags::VIRTIO_BALLOON_S_NR);

        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_SWAP_IN), Some(1000));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_SWAP_OUT), Some(2000));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_MAJFLT), Some(50));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_MINFLT), Some(500));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_MEMFREE), Some(1_000_000));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_MEMTOT), Some(4_000_000));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_AVAIL), Some(2_000_000));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_CACHES), Some(500_000));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_HTLB_PGALLOC), Some(10));
        assert_eq!(dev.get_stat(stat_tags::VIRTIO_BALLOON_S_HTLB_PGFAIL), Some(1));
    }
}
