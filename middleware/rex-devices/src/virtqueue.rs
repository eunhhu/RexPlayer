//! Virtqueue processing helpers
//!
//! Provides types and utilities for working with virtio split virtqueues as
//! defined in the virtio specification (Section 2.6). These helpers handle
//! descriptor chain iteration, used ring updates, and guest memory access
//! through a callback-based interface.

use crate::{DeviceError, DeviceResult};

// ============================================================================
// Virtqueue descriptor flags (virtio spec 2.6.5)
// ============================================================================

/// This marks a buffer as continuing via the `next` field.
pub const VRING_DESC_F_NEXT: u16 = 1;
/// This marks a buffer as device write-only (otherwise device read-only).
pub const VRING_DESC_F_WRITE: u16 = 2;
/// This means the buffer contains a list of buffer descriptors (indirect).
pub const VRING_DESC_F_INDIRECT: u16 = 4;

/// Size of a single virtqueue descriptor in bytes
pub const VRING_DESC_SIZE: usize = 16;
/// Size of the available ring header (flags + idx) in bytes
pub const VRING_AVAIL_HEADER_SIZE: usize = 4;
/// Size of the used ring header (flags + idx) in bytes
pub const VRING_USED_HEADER_SIZE: usize = 4;
/// Size of a single used ring element in bytes
pub const VRING_USED_ELEM_SIZE: usize = 8;

// ============================================================================
// Virtqueue configuration
// ============================================================================

/// Configuration for a single virtqueue, describing the guest physical
/// addresses of the descriptor table, available ring, and used ring.
#[derive(Debug, Clone)]
pub struct VirtqueueConfig {
    /// Number of descriptors (must be a power of 2)
    pub num: u16,
    /// Guest physical address of the descriptor table
    pub desc_addr: u64,
    /// Guest physical address of the available ring
    pub avail_addr: u64,
    /// Guest physical address of the used ring
    pub used_addr: u64,
}

impl VirtqueueConfig {
    /// Validate queue configuration
    pub fn is_valid(&self) -> bool {
        self.num > 0
            && self.num.is_power_of_two()
            && self.desc_addr != 0
            && self.avail_addr != 0
            && self.used_addr != 0
    }
}

// ============================================================================
// Virtqueue descriptor
// ============================================================================

/// A single descriptor from the virtqueue descriptor table.
#[derive(Debug, Clone, Copy)]
pub struct VirtqDesc {
    /// Guest physical address of the buffer
    pub addr: u64,
    /// Length of the buffer in bytes
    pub len: u32,
    /// Descriptor flags (VRING_DESC_F_*)
    pub flags: u16,
    /// Index of the next descriptor in the chain (if VRING_DESC_F_NEXT is set)
    pub next: u16,
}

impl VirtqDesc {
    /// Parse a descriptor from a 16-byte buffer (little-endian)
    pub fn from_bytes(bytes: &[u8; VRING_DESC_SIZE]) -> Self {
        Self {
            addr: u64::from_le_bytes([
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                bytes[7],
            ]),
            len: u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]),
            flags: u16::from_le_bytes([bytes[12], bytes[13]]),
            next: u16::from_le_bytes([bytes[14], bytes[15]]),
        }
    }

    /// Serialize the descriptor to a 16-byte buffer (little-endian)
    pub fn to_bytes(&self) -> [u8; VRING_DESC_SIZE] {
        let mut buf = [0u8; VRING_DESC_SIZE];
        buf[0..8].copy_from_slice(&self.addr.to_le_bytes());
        buf[8..12].copy_from_slice(&self.len.to_le_bytes());
        buf[12..14].copy_from_slice(&self.flags.to_le_bytes());
        buf[14..16].copy_from_slice(&self.next.to_le_bytes());
        buf
    }

    /// Check whether this descriptor chains to another
    pub fn has_next(&self) -> bool {
        self.flags & VRING_DESC_F_NEXT != 0
    }

    /// Check whether this is a device-writable buffer
    pub fn is_write_only(&self) -> bool {
        self.flags & VRING_DESC_F_WRITE != 0
    }

    /// Check whether this is an indirect descriptor
    pub fn is_indirect(&self) -> bool {
        self.flags & VRING_DESC_F_INDIRECT != 0
    }
}

// ============================================================================
// Used ring element
// ============================================================================

/// An element in the used ring, reporting a completed descriptor chain.
#[derive(Debug, Clone, Copy)]
pub struct VirtqUsedElem {
    /// Index of the start of the used descriptor chain
    pub id: u32,
    /// Total number of bytes written into the descriptor chain buffers
    pub len: u32,
}

impl VirtqUsedElem {
    /// Serialize to 8 bytes (little-endian)
    pub fn to_bytes(&self) -> [u8; VRING_USED_ELEM_SIZE] {
        let mut buf = [0u8; VRING_USED_ELEM_SIZE];
        buf[0..4].copy_from_slice(&self.id.to_le_bytes());
        buf[4..8].copy_from_slice(&self.len.to_le_bytes());
        buf
    }

    /// Parse from an 8-byte buffer (little-endian)
    pub fn from_bytes(bytes: &[u8; VRING_USED_ELEM_SIZE]) -> Self {
        Self {
            id: u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            len: u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]),
        }
    }
}

// ============================================================================
// Guest memory access trait
// ============================================================================

/// Abstraction for reading/writing guest physical memory.
///
/// This trait allows the virtqueue code to access guest memory without being
/// coupled to a specific memory management implementation. The VMM provides
/// an implementation that translates GPAs to HVAs and performs the access.
pub trait GuestMemoryAccess {
    /// Read `len` bytes from the guest physical address `gpa`.
    fn read_guest(&self, gpa: u64, buf: &mut [u8]) -> DeviceResult<()>;

    /// Write `buf` to the guest physical address `gpa`.
    fn write_guest(&self, gpa: u64, buf: &[u8]) -> DeviceResult<()>;
}

// ============================================================================
// Virtqueue helper
// ============================================================================

/// Helper for processing a split virtqueue.
///
/// Manages the last-seen available index and provides methods to pop
/// descriptor chains and push used entries.
pub struct Virtqueue {
    config: VirtqueueConfig,
    /// Host-side tracking of the last consumed available-ring index
    last_avail_idx: u16,
}

impl Virtqueue {
    /// Create a new virtqueue helper from the given configuration.
    pub fn new(config: VirtqueueConfig) -> DeviceResult<Self> {
        if !config.is_valid() {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            config,
            last_avail_idx: 0,
        })
    }

    /// Get the queue configuration
    pub fn config(&self) -> &VirtqueueConfig {
        &self.config
    }

    /// Get the queue size (number of descriptors)
    pub fn size(&self) -> u16 {
        self.config.num
    }

    /// Read a single descriptor from the descriptor table.
    pub fn read_desc(
        &self,
        mem: &dyn GuestMemoryAccess,
        index: u16,
    ) -> DeviceResult<VirtqDesc> {
        if index >= self.config.num {
            return Err(DeviceError::InvalidDescriptor);
        }

        let offset = (index as u64) * (VRING_DESC_SIZE as u64);
        let gpa = self.config.desc_addr + offset;

        let mut buf = [0u8; VRING_DESC_SIZE];
        mem.read_guest(gpa, &mut buf)?;

        Ok(VirtqDesc::from_bytes(&buf))
    }

    /// Read the current available ring index from guest memory.
    fn read_avail_idx(&self, mem: &dyn GuestMemoryAccess) -> DeviceResult<u16> {
        // avail ring layout: u16 flags, u16 idx, u16 ring[num], ...
        let gpa = self.config.avail_addr + 2; // skip flags
        let mut buf = [0u8; 2];
        mem.read_guest(gpa, &mut buf)?;
        Ok(u16::from_le_bytes(buf))
    }

    /// Read an entry from the available ring at the given ring index.
    fn read_avail_ring(
        &self,
        mem: &dyn GuestMemoryAccess,
        ring_idx: u16,
    ) -> DeviceResult<u16> {
        let wrapped = ring_idx % self.config.num;
        // avail ring entry offset: header (4 bytes) + wrapped * 2
        let gpa = self.config.avail_addr
            + VRING_AVAIL_HEADER_SIZE as u64
            + (wrapped as u64) * 2;

        let mut buf = [0u8; 2];
        mem.read_guest(gpa, &mut buf)?;
        Ok(u16::from_le_bytes(buf))
    }

    /// Check whether there are new available buffers to process.
    pub fn has_available(&self, mem: &dyn GuestMemoryAccess) -> DeviceResult<bool> {
        let avail_idx = self.read_avail_idx(mem)?;
        Ok(avail_idx != self.last_avail_idx)
    }

    /// Pop the next available descriptor chain head index.
    ///
    /// Returns `None` if no new buffers are available. Advances the
    /// internal `last_avail_idx` counter.
    pub fn pop_avail(
        &mut self,
        mem: &dyn GuestMemoryAccess,
    ) -> DeviceResult<Option<u16>> {
        let avail_idx = self.read_avail_idx(mem)?;

        if avail_idx == self.last_avail_idx {
            return Ok(None);
        }

        let head = self.read_avail_ring(mem, self.last_avail_idx)?;
        self.last_avail_idx = self.last_avail_idx.wrapping_add(1);

        Ok(Some(head))
    }

    /// Iterate over the descriptor chain starting at `head`.
    ///
    /// Returns a `DescChainIter` that yields each `VirtqDesc` in the chain
    /// by following the `next` pointers.
    pub fn desc_chain<'a>(
        &'a self,
        mem: &'a dyn GuestMemoryAccess,
        head: u16,
    ) -> DescChainIter<'a> {
        DescChainIter {
            queue: self,
            mem,
            next_index: Some(head),
            visited: 0,
        }
    }

    /// Read the current used ring index from guest memory.
    fn read_used_idx(&self, mem: &dyn GuestMemoryAccess) -> DeviceResult<u16> {
        let gpa = self.config.used_addr + 2; // skip flags
        let mut buf = [0u8; 2];
        mem.read_guest(gpa, &mut buf)?;
        Ok(u16::from_le_bytes(buf))
    }

    /// Write the used ring index to guest memory.
    fn write_used_idx(
        &self,
        mem: &dyn GuestMemoryAccess,
        idx: u16,
    ) -> DeviceResult<()> {
        let gpa = self.config.used_addr + 2;
        mem.write_guest(gpa, &idx.to_le_bytes())
    }

    /// Push a completed descriptor chain into the used ring.
    ///
    /// Writes the used element and advances the used ring index.
    pub fn push_used(
        &self,
        mem: &dyn GuestMemoryAccess,
        elem: VirtqUsedElem,
    ) -> DeviceResult<()> {
        let used_idx = self.read_used_idx(mem)?;
        let wrapped = used_idx % self.config.num;

        // used ring element offset: header (4 bytes) + wrapped * 8
        let elem_gpa = self.config.used_addr
            + VRING_USED_HEADER_SIZE as u64
            + (wrapped as u64) * (VRING_USED_ELEM_SIZE as u64);

        mem.write_guest(elem_gpa, &elem.to_bytes())?;

        // Update the used index
        let new_idx = used_idx.wrapping_add(1);
        self.write_used_idx(mem, new_idx)?;

        Ok(())
    }
}

// ============================================================================
// Descriptor chain iterator
// ============================================================================

/// Iterator over a descriptor chain in a virtqueue.
///
/// Follows the `next` pointers through the descriptor table, yielding each
/// descriptor in sequence. Stops when a descriptor without `VRING_DESC_F_NEXT`
/// is encountered, or after visiting `queue.size()` descriptors (to prevent
/// infinite loops from malicious guests).
pub struct DescChainIter<'a> {
    queue: &'a Virtqueue,
    mem: &'a dyn GuestMemoryAccess,
    next_index: Option<u16>,
    /// Safety counter to detect loops
    visited: u16,
}

impl<'a> Iterator for DescChainIter<'a> {
    type Item = DeviceResult<VirtqDesc>;

    fn next(&mut self) -> Option<Self::Item> {
        let index = self.next_index?;

        // Guard against loops: we should never visit more descriptors than
        // the queue size
        if self.visited >= self.queue.size() {
            return Some(Err(DeviceError::InvalidDescriptor));
        }
        self.visited += 1;

        let desc = match self.queue.read_desc(self.mem, index) {
            Ok(d) => d,
            Err(e) => return Some(Err(e)),
        };

        if desc.has_next() {
            self.next_index = Some(desc.next);
        } else {
            self.next_index = None;
        }

        Some(Ok(desc))
    }
}

// ============================================================================
// Convenience: collect readable and writable buffers
// ============================================================================

/// A buffer segment from a descriptor chain, categorized as readable or
/// writable from the device's perspective.
#[derive(Debug, Clone)]
pub struct BufferSegment {
    /// Guest physical address of the buffer
    pub addr: u64,
    /// Length in bytes
    pub len: u32,
}

/// Split a descriptor chain into device-readable and device-writable segments.
///
/// Per the virtio spec, all readable descriptors come before all writable
/// descriptors in the chain. This function collects them into two separate
/// vectors.
pub fn collect_chain_buffers(
    queue: &Virtqueue,
    mem: &dyn GuestMemoryAccess,
    head: u16,
) -> DeviceResult<(Vec<BufferSegment>, Vec<BufferSegment>)> {
    let mut readable = Vec::new();
    let mut writable = Vec::new();

    for result in queue.desc_chain(mem, head) {
        let desc = result?;
        let segment = BufferSegment {
            addr: desc.addr,
            len: desc.len,
        };

        if desc.is_write_only() {
            writable.push(segment);
        } else {
            readable.push(segment);
        }
    }

    Ok((readable, writable))
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// In-memory mock for guest physical memory
    struct MockMemory {
        data: std::cell::RefCell<HashMap<u64, Vec<u8>>>,
    }

    impl MockMemory {
        fn new() -> Self {
            Self {
                data: std::cell::RefCell::new(HashMap::new()),
            }
        }

        /// Write bytes directly (for test setup)
        fn store(&self, gpa: u64, bytes: &[u8]) {
            let mut data = self.data.borrow_mut();
            for (i, &b) in bytes.iter().enumerate() {
                data.entry(gpa + i as u64)
                    .and_modify(|v| {
                        v.clear();
                        v.push(b);
                    })
                    .or_insert_with(|| vec![b]);
            }
        }

        /// Read bytes directly (for test assertions)
        fn load(&self, gpa: u64, len: usize) -> Vec<u8> {
            let data = self.data.borrow();
            (0..len)
                .map(|i| {
                    data.get(&(gpa + i as u64))
                        .and_then(|v| v.first().copied())
                        .unwrap_or(0)
                })
                .collect()
        }
    }

    impl GuestMemoryAccess for MockMemory {
        fn read_guest(&self, gpa: u64, buf: &mut [u8]) -> DeviceResult<()> {
            let data = self.data.borrow();
            for (i, b) in buf.iter_mut().enumerate() {
                *b = data
                    .get(&(gpa + i as u64))
                    .and_then(|v| v.first().copied())
                    .unwrap_or(0);
            }
            Ok(())
        }

        fn write_guest(&self, gpa: u64, buf: &[u8]) -> DeviceResult<()> {
            let mut data = self.data.borrow_mut();
            for (i, &b) in buf.iter().enumerate() {
                data.insert(gpa + i as u64, vec![b]);
            }
            Ok(())
        }
    }

    // Layout constants for a 4-entry queue at fixed GPAs
    const DESC_ADDR: u64 = 0x1000;
    const AVAIL_ADDR: u64 = 0x2000;
    const USED_ADDR: u64 = 0x3000;

    fn make_config() -> VirtqueueConfig {
        VirtqueueConfig {
            num: 4,
            desc_addr: DESC_ADDR,
            avail_addr: AVAIL_ADDR,
            used_addr: USED_ADDR,
        }
    }

    /// Place a descriptor in the mock memory at the given index
    fn place_desc(mem: &MockMemory, index: u16, desc: &VirtqDesc) {
        let gpa = DESC_ADDR + (index as u64) * VRING_DESC_SIZE as u64;
        mem.store(gpa, &desc.to_bytes());
    }

    /// Set the available ring index and ring entries
    fn set_avail(mem: &MockMemory, idx: u16, entries: &[u16]) {
        // flags = 0
        mem.store(AVAIL_ADDR, &0u16.to_le_bytes());
        // idx
        mem.store(AVAIL_ADDR + 2, &idx.to_le_bytes());
        // ring entries
        for (i, &entry) in entries.iter().enumerate() {
            mem.store(
                AVAIL_ADDR + VRING_AVAIL_HEADER_SIZE as u64 + (i as u64) * 2,
                &entry.to_le_bytes(),
            );
        }
    }

    /// Initialize the used ring (flags=0, idx=0)
    fn init_used(mem: &MockMemory) {
        mem.store(USED_ADDR, &0u16.to_le_bytes());
        mem.store(USED_ADDR + 2, &0u16.to_le_bytes());
    }

    #[test]
    fn config_validation() {
        let valid = make_config();
        assert!(valid.is_valid());

        let bad_num = VirtqueueConfig { num: 0, ..valid.clone() };
        assert!(!bad_num.is_valid());

        let bad_pow2 = VirtqueueConfig { num: 3, ..valid.clone() };
        assert!(!bad_pow2.is_valid());

        let bad_addr = VirtqueueConfig {
            desc_addr: 0,
            ..valid.clone()
        };
        assert!(!bad_addr.is_valid());
    }

    #[test]
    fn desc_serialization_roundtrip() {
        let desc = VirtqDesc {
            addr: 0xDEAD_BEEF_CAFE_0000,
            len: 4096,
            flags: VRING_DESC_F_NEXT | VRING_DESC_F_WRITE,
            next: 7,
        };
        let bytes = desc.to_bytes();
        let parsed = VirtqDesc::from_bytes(&bytes);

        assert_eq!(parsed.addr, desc.addr);
        assert_eq!(parsed.len, desc.len);
        assert_eq!(parsed.flags, desc.flags);
        assert_eq!(parsed.next, desc.next);
        assert!(parsed.has_next());
        assert!(parsed.is_write_only());
        assert!(!parsed.is_indirect());
    }

    #[test]
    fn pop_avail_single() {
        let mem = MockMemory::new();
        let config = make_config();

        // Place one descriptor
        place_desc(&mem, 0, &VirtqDesc {
            addr: 0x5000,
            len: 512,
            flags: 0,
            next: 0,
        });

        // Available ring: 1 entry at ring[0] = descriptor index 0
        set_avail(&mem, 1, &[0]);
        init_used(&mem);

        let mut queue = Virtqueue::new(config).unwrap();

        // Pop the available entry
        let head = queue.pop_avail(&mem).unwrap();
        assert_eq!(head, Some(0));

        // No more available
        let head2 = queue.pop_avail(&mem).unwrap();
        assert_eq!(head2, None);
    }

    #[test]
    fn desc_chain_iteration() {
        let mem = MockMemory::new();
        let config = make_config();

        // Chain: desc 0 -> desc 1 -> desc 2 (end)
        place_desc(&mem, 0, &VirtqDesc {
            addr: 0x5000,
            len: 256,
            flags: VRING_DESC_F_NEXT,
            next: 1,
        });
        place_desc(&mem, 1, &VirtqDesc {
            addr: 0x5100,
            len: 128,
            flags: VRING_DESC_F_NEXT | VRING_DESC_F_WRITE,
            next: 2,
        });
        place_desc(&mem, 2, &VirtqDesc {
            addr: 0x5200,
            len: 64,
            flags: VRING_DESC_F_WRITE,
            next: 0,
        });

        let queue = Virtqueue::new(config).unwrap();
        let descs: Vec<VirtqDesc> = queue
            .desc_chain(&mem, 0)
            .collect::<DeviceResult<Vec<_>>>()
            .unwrap();

        assert_eq!(descs.len(), 3);
        assert_eq!(descs[0].addr, 0x5000);
        assert_eq!(descs[0].len, 256);
        assert!(!descs[0].is_write_only());
        assert_eq!(descs[1].addr, 0x5100);
        assert!(descs[1].is_write_only());
        assert_eq!(descs[2].addr, 0x5200);
        assert!(descs[2].is_write_only());
    }

    #[test]
    fn collect_chain_separates_rw() {
        let mem = MockMemory::new();
        let config = make_config();

        // Readable -> Writable chain
        place_desc(&mem, 0, &VirtqDesc {
            addr: 0x5000,
            len: 512,
            flags: VRING_DESC_F_NEXT,
            next: 1,
        });
        place_desc(&mem, 1, &VirtqDesc {
            addr: 0x6000,
            len: 1,
            flags: VRING_DESC_F_WRITE,
            next: 0,
        });

        let queue = Virtqueue::new(config).unwrap();
        let (readable, writable) = collect_chain_buffers(&queue, &mem, 0).unwrap();

        assert_eq!(readable.len(), 1);
        assert_eq!(readable[0].addr, 0x5000);
        assert_eq!(readable[0].len, 512);

        assert_eq!(writable.len(), 1);
        assert_eq!(writable[0].addr, 0x6000);
        assert_eq!(writable[0].len, 1);
    }

    #[test]
    fn push_used_updates_ring() {
        let mem = MockMemory::new();
        let config = make_config();

        init_used(&mem);

        let queue = Virtqueue::new(config).unwrap();

        // Push a used element
        queue
            .push_used(&mem, VirtqUsedElem { id: 0, len: 256 })
            .unwrap();

        // Check the used index was incremented
        let used_idx_bytes = mem.load(USED_ADDR + 2, 2);
        let used_idx = u16::from_le_bytes([used_idx_bytes[0], used_idx_bytes[1]]);
        assert_eq!(used_idx, 1);

        // Check the used element was written
        let elem_bytes = mem.load(
            USED_ADDR + VRING_USED_HEADER_SIZE as u64,
            VRING_USED_ELEM_SIZE,
        );
        let elem = VirtqUsedElem::from_bytes(elem_bytes.as_slice().try_into().unwrap());
        assert_eq!(elem.id, 0);
        assert_eq!(elem.len, 256);

        // Push another
        queue
            .push_used(&mem, VirtqUsedElem { id: 1, len: 128 })
            .unwrap();

        let used_idx_bytes = mem.load(USED_ADDR + 2, 2);
        let used_idx = u16::from_le_bytes([used_idx_bytes[0], used_idx_bytes[1]]);
        assert_eq!(used_idx, 2);
    }

    #[test]
    fn used_elem_roundtrip() {
        let elem = VirtqUsedElem { id: 42, len: 1024 };
        let bytes = elem.to_bytes();
        let parsed = VirtqUsedElem::from_bytes(&bytes);
        assert_eq!(parsed.id, 42);
        assert_eq!(parsed.len, 1024);
    }
}
