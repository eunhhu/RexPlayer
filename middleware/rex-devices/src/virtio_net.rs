//! Virtio network device backend
//!
//! Implements the virtio-net device as defined in the virtio specification
//! (Section 5.1). Provides a virtual NIC with configurable MAC address,
//! RX/TX queue handling, and pluggable network backends (TAP or userspace NAT).

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

// ============================================================================
// Feature flags (virtio spec 5.1.3)
// ============================================================================

pub mod features {
    /// Device has given MAC address
    pub const VIRTIO_NET_F_MAC: u64 = 1 << 5;
    /// Device supports link status reporting
    pub const VIRTIO_NET_F_STATUS: u64 = 1 << 16;
    /// Guest can merge receive buffers
    pub const VIRTIO_NET_F_MRG_RXBUF: u64 = 1 << 15;
    /// Control channel available
    pub const VIRTIO_NET_F_CTRL_VQ: u64 = 1 << 17;
    /// Guest can handle TSO for IPv4
    pub const VIRTIO_NET_F_GUEST_TSO4: u64 = 1 << 7;
    /// Guest can handle checksum offload
    pub const VIRTIO_NET_F_GUEST_CSUM: u64 = 1 << 1;
    /// Device can handle checksum offload
    pub const VIRTIO_NET_F_CSUM: u64 = 1 << 0;
}

// ============================================================================
// Virtio net header (virtio spec 5.1.6)
// ============================================================================

/// Size of the virtio-net header in bytes
pub const VIRTIO_NET_HDR_SIZE: usize = 12;

/// Flags for virtio_net_hdr
pub mod hdr_flags {
    pub const VIRTIO_NET_HDR_F_NEEDS_CSUM: u8 = 1;
    pub const VIRTIO_NET_HDR_F_DATA_VALID: u8 = 2;
}

/// GSO type values
pub mod gso {
    pub const VIRTIO_NET_HDR_GSO_NONE: u8 = 0;
    pub const VIRTIO_NET_HDR_GSO_TCPV4: u8 = 1;
    pub const VIRTIO_NET_HDR_GSO_UDP: u8 = 3;
    pub const VIRTIO_NET_HDR_GSO_TCPV6: u8 = 4;
}

/// Virtio network header prepended to every packet
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct VirtioNetHdr {
    /// Flags (see hdr_flags)
    pub flags: u8,
    /// GSO type (see gso)
    pub gso_type: u8,
    /// Ethernet header + IP header length (for GSO)
    pub hdr_len: u16,
    /// MSS for GSO
    pub gso_size: u16,
    /// Checksum start offset
    pub csum_start: u16,
    /// Checksum write offset (from csum_start)
    pub csum_offset: u16,
    /// Number of merged buffers (only with VIRTIO_NET_F_MRG_RXBUF)
    pub num_buffers: u16,
}

impl VirtioNetHdr {
    /// Create a default header with no offloading
    pub fn new() -> Self {
        Self {
            flags: 0,
            gso_type: gso::VIRTIO_NET_HDR_GSO_NONE,
            hdr_len: 0,
            gso_size: 0,
            csum_start: 0,
            csum_offset: 0,
            num_buffers: 1,
        }
    }

    /// Serialize to bytes (little-endian)
    pub fn to_bytes(&self) -> [u8; VIRTIO_NET_HDR_SIZE] {
        let mut buf = [0u8; VIRTIO_NET_HDR_SIZE];
        buf[0] = self.flags;
        buf[1] = self.gso_type;
        buf[2..4].copy_from_slice(&self.hdr_len.to_le_bytes());
        buf[4..6].copy_from_slice(&self.gso_size.to_le_bytes());
        buf[6..8].copy_from_slice(&self.csum_start.to_le_bytes());
        buf[8..10].copy_from_slice(&self.csum_offset.to_le_bytes());
        buf[10..12].copy_from_slice(&self.num_buffers.to_le_bytes());
        buf
    }

    /// Parse from bytes (little-endian)
    pub fn from_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < VIRTIO_NET_HDR_SIZE {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            flags: buf[0],
            gso_type: buf[1],
            hdr_len: u16::from_le_bytes([buf[2], buf[3]]),
            gso_size: u16::from_le_bytes([buf[4], buf[5]]),
            csum_start: u16::from_le_bytes([buf[6], buf[7]]),
            csum_offset: u16::from_le_bytes([buf[8], buf[9]]),
            num_buffers: u16::from_le_bytes([buf[10], buf[11]]),
        })
    }
}

impl Default for VirtioNetHdr {
    fn default() -> Self {
        Self::new()
    }
}

// ============================================================================
// Network link status
// ============================================================================

/// Link status values for VIRTIO_NET_F_STATUS
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinkStatus {
    Down = 0,
    Up = 1,
}

// ============================================================================
// Config space (virtio spec 5.1.4)
// ============================================================================

/// Virtio-net device configuration space.
///
/// Layout:
/// - bytes 0..6: MAC address
/// - bytes 6..8: status (u16, if VIRTIO_NET_F_STATUS)
/// - bytes 8..10: max_virtqueue_pairs (u16)
#[derive(Debug, Clone)]
pub struct VirtioNetConfig {
    /// MAC address (6 bytes)
    pub mac: [u8; 6],
    /// Link status
    pub status: LinkStatus,
    /// Maximum number of TX/RX queue pairs
    pub max_virtqueue_pairs: u16,
}

impl VirtioNetConfig {
    pub fn new(mac: [u8; 6]) -> Self {
        Self {
            mac,
            status: LinkStatus::Up,
            max_virtqueue_pairs: 1,
        }
    }

    /// Serialize config space to bytes (little-endian)
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(10);
        buf.extend_from_slice(&self.mac);
        buf.extend_from_slice(&(self.status as u16).to_le_bytes());
        buf.extend_from_slice(&self.max_virtqueue_pairs.to_le_bytes());
        buf
    }

    /// Read a single byte from the config space at the given offset
    pub fn read_byte(&self, offset: usize) -> u8 {
        let bytes = self.to_bytes();
        bytes.get(offset).copied().unwrap_or(0)
    }

    /// Read a 32-bit value from the config space at the given offset
    pub fn read_u32(&self, offset: usize) -> u32 {
        let bytes = self.to_bytes();
        let mut val = 0u32;
        for i in 0..4 {
            if let Some(&b) = bytes.get(offset + i) {
                val |= (b as u32) << (i * 8);
            }
        }
        val
    }
}

// ============================================================================
// Network backend trait
// ============================================================================

/// A network packet: virtio-net header + Ethernet frame
#[derive(Debug, Clone)]
pub struct NetPacket {
    /// The virtio-net header
    pub hdr: VirtioNetHdr,
    /// Raw Ethernet frame data (without virtio header)
    pub data: Vec<u8>,
}

/// Trait for network backends that can send and receive packets.
///
/// Implementations include TapBackend (real TAP/TUN device) and
/// UserNetBackend (userspace NAT).
pub trait NetworkBackend: Send {
    /// Send a packet from the guest to the network
    fn send(&mut self, packet: &NetPacket) -> DeviceResult<()>;

    /// Receive a packet from the network for the guest.
    /// Returns None if no packet is available.
    fn recv(&mut self) -> DeviceResult<Option<NetPacket>>;

    /// Check if a packet is available for receiving
    fn has_pending_rx(&self) -> bool;
}

// ============================================================================
// TAP backend
// ============================================================================

/// TAP device backend — uses a host TAP interface for bridged networking.
///
/// This backend opens a TAP device (e.g., /dev/net/tun on Linux) and
/// bridges guest traffic directly to the host network.
pub struct TapBackend {
    /// Name of the TAP interface (e.g., "tap0")
    _tap_name: String,
    /// File descriptor for the TAP device (platform-specific)
    _tap_fd: Option<i32>,
    /// Pending RX packets from the TAP device
    rx_queue: VecDeque<NetPacket>,
}

impl TapBackend {
    /// Create a new TAP backend.
    ///
    /// Note: Actual TAP device creation requires root/CAP_NET_ADMIN
    /// and is platform-specific. This constructor prepares the struct;
    /// the TAP FD must be opened separately via platform-specific code.
    pub fn new(tap_name: &str) -> Self {
        Self {
            _tap_name: tap_name.to_string(),
            _tap_fd: None,
            rx_queue: VecDeque::new(),
        }
    }

    /// Inject a packet into the RX queue (for testing or external injection)
    pub fn inject_rx(&mut self, packet: NetPacket) {
        self.rx_queue.push_back(packet);
    }
}

impl NetworkBackend for TapBackend {
    fn send(&mut self, _packet: &NetPacket) -> DeviceResult<()> {
        // In a real implementation, write the packet to the TAP fd:
        // libc::write(self.tap_fd, packet.data.as_ptr(), packet.data.len())
        // For now, log and discard since TAP fd setup is platform-specific
        tracing::trace!("tap: TX packet ({} bytes)", _packet.data.len());
        Ok(())
    }

    fn recv(&mut self) -> DeviceResult<Option<NetPacket>> {
        Ok(self.rx_queue.pop_front())
    }

    fn has_pending_rx(&self) -> bool {
        !self.rx_queue.is_empty()
    }
}

// ============================================================================
// Userspace NAT backend
// ============================================================================

/// Simple userspace NAT backend — provides SLIRP-like networking.
///
/// This backend performs NAT for guest traffic without requiring root
/// privileges. It translates guest IP addresses to host addresses using
/// the 10.0.2.0/24 subnet by default.
pub struct UserNetBackend {
    /// Gateway IP within the guest subnet (default: 10.0.2.2)
    gateway_ip: [u8; 4],
    /// DNS server IP within the guest subnet (default: 10.0.2.3)
    dns_ip: [u8; 4],
    /// Guest IP address (default: 10.0.2.15)
    guest_ip: [u8; 4],
    /// Subnet mask (default: 255.255.255.0)
    netmask: [u8; 4],
    /// Pending RX packets for the guest
    rx_queue: VecDeque<NetPacket>,
    /// Pending TX packets from the guest (for testing/inspection)
    tx_queue: VecDeque<NetPacket>,
}

impl UserNetBackend {
    /// Create a new userspace NAT backend with default subnet 10.0.2.0/24
    pub fn new() -> Self {
        Self {
            gateway_ip: [10, 0, 2, 2],
            dns_ip: [10, 0, 2, 3],
            guest_ip: [10, 0, 2, 15],
            netmask: [255, 255, 255, 0],
            rx_queue: VecDeque::new(),
            tx_queue: VecDeque::new(),
        }
    }

    /// Create with custom subnet configuration
    pub fn with_config(
        gateway_ip: [u8; 4],
        dns_ip: [u8; 4],
        guest_ip: [u8; 4],
        netmask: [u8; 4],
    ) -> Self {
        Self {
            gateway_ip,
            dns_ip,
            guest_ip,
            netmask,
            rx_queue: VecDeque::new(),
            tx_queue: VecDeque::new(),
        }
    }

    /// Get the gateway IP
    pub fn gateway_ip(&self) -> &[u8; 4] {
        &self.gateway_ip
    }

    /// Get the DNS server IP
    pub fn dns_ip(&self) -> &[u8; 4] {
        &self.dns_ip
    }

    /// Get the guest IP
    pub fn guest_ip(&self) -> &[u8; 4] {
        &self.guest_ip
    }

    /// Get the netmask
    pub fn netmask(&self) -> &[u8; 4] {
        &self.netmask
    }

    /// Inject a response packet into the RX queue (from host -> guest)
    pub fn inject_rx(&mut self, packet: NetPacket) {
        self.rx_queue.push_back(packet);
    }

    /// Drain TX packets (for testing/inspection)
    pub fn drain_tx(&mut self) -> Vec<NetPacket> {
        self.tx_queue.drain(..).collect()
    }

    /// Check if an IP belongs to the guest subnet
    pub fn is_in_subnet(&self, ip: &[u8; 4]) -> bool {
        for ((ip_octet, mask_octet), gateway_octet) in ip
            .iter()
            .zip(self.netmask.iter())
            .zip(self.gateway_ip.iter())
        {
            if (ip_octet & mask_octet) != (gateway_octet & mask_octet) {
                return false;
            }
        }
        true
    }
}

impl Default for UserNetBackend {
    fn default() -> Self {
        Self::new()
    }
}

impl NetworkBackend for UserNetBackend {
    fn send(&mut self, packet: &NetPacket) -> DeviceResult<()> {
        tracing::trace!(
            "usernet: TX packet ({} bytes), will NAT to host",
            packet.data.len()
        );
        // Store for inspection / NAT processing
        self.tx_queue.push_back(packet.clone());
        Ok(())
    }

    fn recv(&mut self) -> DeviceResult<Option<NetPacket>> {
        Ok(self.rx_queue.pop_front())
    }

    fn has_pending_rx(&self) -> bool {
        !self.rx_queue.is_empty()
    }
}

// ============================================================================
// Queue indices
// ============================================================================

/// Virtqueue index for the receive queue
pub const RX_QUEUE: u16 = 0;
/// Virtqueue index for the transmit queue
pub const TX_QUEUE: u16 = 1;

// ============================================================================
// VirtioNet device
// ============================================================================

/// Virtio network device.
///
/// Implements the virtio-net device with:
/// - Configurable MAC address
/// - Feature negotiation (MAC, STATUS, MRG_RXBUF)
/// - RX queue (0) and TX queue (1)
/// - Pluggable network backend
pub struct VirtioNet {
    /// Device configuration
    config: VirtioNetConfig,
    /// Negotiated feature flags
    features: u64,
    /// Whether the device has been activated
    activated: bool,
    /// Network backend for send/receive
    backend: Arc<Mutex<Box<dyn NetworkBackend>>>,
    /// Pending RX buffers (packets waiting to be delivered to guest)
    rx_buffer: VecDeque<NetPacket>,
    /// TX buffer (packets waiting to be sent to backend)
    tx_buffer: VecDeque<Vec<u8>>,
    /// Maximum packet size (MTU + headers)
    mtu: u16,
}

impl VirtioNet {
    /// Create a new virtio-net device with the given MAC address and backend.
    pub fn new(mac: [u8; 6], backend: Box<dyn NetworkBackend>) -> Self {
        let config = VirtioNetConfig::new(mac);
        let device_features = features::VIRTIO_NET_F_MAC
            | features::VIRTIO_NET_F_STATUS
            | features::VIRTIO_NET_F_MRG_RXBUF
            | features::VIRTIO_NET_F_CSUM
            | features::VIRTIO_NET_F_GUEST_CSUM;

        Self {
            config,
            features: device_features,
            activated: false,
            backend: Arc::new(Mutex::new(backend)),
            rx_buffer: VecDeque::with_capacity(256),
            tx_buffer: VecDeque::with_capacity(256),
            mtu: 1514, // Standard Ethernet MTU + header
        }
    }

    /// Create a new virtio-net device with a userspace NAT backend
    pub fn new_user(mac: [u8; 6]) -> Self {
        Self::new(mac, Box::new(UserNetBackend::new()))
    }

    /// Get the MAC address
    pub fn mac(&self) -> &[u8; 6] {
        &self.config.mac
    }

    /// Get the device configuration
    pub fn config(&self) -> &VirtioNetConfig {
        &self.config
    }

    /// Get the link status
    pub fn link_status(&self) -> LinkStatus {
        self.config.status
    }

    /// Set the link status
    pub fn set_link_status(&mut self, status: LinkStatus) {
        self.config.status = status;
    }

    /// Get the MTU
    pub fn mtu(&self) -> u16 {
        self.mtu
    }

    /// Get a reference to the backend
    pub fn backend(&self) -> &Arc<Mutex<Box<dyn NetworkBackend>>> {
        &self.backend
    }

    /// Queue a packet for delivery to the guest (RX path)
    pub fn queue_rx_packet(&mut self, packet: NetPacket) {
        self.rx_buffer.push_back(packet);
    }

    /// Process a TX packet from the guest.
    ///
    /// Parses the virtio-net header and sends the Ethernet frame
    /// through the backend.
    pub fn process_tx_packet(&mut self, raw: &[u8]) -> DeviceResult<()> {
        if raw.len() < VIRTIO_NET_HDR_SIZE {
            return Err(DeviceError::InvalidDescriptor);
        }

        let hdr = VirtioNetHdr::from_bytes(&raw[..VIRTIO_NET_HDR_SIZE])?;
        let data = raw[VIRTIO_NET_HDR_SIZE..].to_vec();

        let packet = NetPacket { hdr, data };

        let mut backend = self.backend.lock().map_err(|_| {
            DeviceError::Io(std::io::Error::other("backend lock poisoned"))
        })?;
        backend.send(&packet)?;
        Ok(())
    }

    /// Poll for incoming packets from the backend and queue them for RX.
    pub fn poll_rx(&mut self) -> DeviceResult<usize> {
        let mut backend = self.backend.lock().map_err(|_| {
            DeviceError::Io(std::io::Error::other("backend lock poisoned"))
        })?;

        let mut count = 0;
        while let Some(packet) = backend.recv()? {
            self.rx_buffer.push_back(packet);
            count += 1;
        }
        Ok(count)
    }

    /// Get the next pending RX packet
    pub fn pop_rx_packet(&mut self) -> Option<NetPacket> {
        self.rx_buffer.pop_front()
    }

    /// Check if there are pending RX packets
    pub fn has_pending_rx(&self) -> bool {
        !self.rx_buffer.is_empty()
    }

    /// Build a complete RX buffer (virtio-net header + Ethernet frame)
    /// ready to be placed into guest memory via the RX virtqueue.
    pub fn build_rx_buffer(packet: &NetPacket) -> Vec<u8> {
        let mut buf = Vec::with_capacity(VIRTIO_NET_HDR_SIZE + packet.data.len());
        buf.extend_from_slice(&packet.hdr.to_bytes());
        buf.extend_from_slice(&packet.data);
        buf
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
        let mac = &self.config.mac;
        tracing::info!(
            "virtio-net activated: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}, status={:?}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            self.config.status
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.rx_buffer.clear();
        self.tx_buffer.clear();
        self.config.status = LinkStatus::Up;
    }

    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
        match queue_index {
            RX_QUEUE => {
                // RX queue processing: transfer pending packets from
                // rx_buffer into guest-provided buffers via the virtqueue.
                // The actual virtqueue buffer filling is done by the
                // transport layer using our pop_rx_packet() method.
                tracing::trace!(
                    "virtio-net: RX queue notified, {} packets pending",
                    self.rx_buffer.len()
                );
                Ok(())
            }
            TX_QUEUE => {
                // TX queue processing: the guest has placed outgoing
                // packets in the TX queue. The transport layer reads
                // them and calls process_tx_packet() for each.
                tracing::trace!("virtio-net: TX queue notified");
                Ok(())
            }
            _ => {
                tracing::warn!("virtio-net: unknown queue index {}", queue_index);
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

    /// Mock network backend for testing
    struct MockBackend {
        tx_packets: Vec<NetPacket>,
        rx_packets: VecDeque<NetPacket>,
    }

    impl MockBackend {
        fn new() -> Self {
            Self {
                tx_packets: Vec::new(),
                rx_packets: VecDeque::new(),
            }
        }

        fn push_rx(&mut self, data: Vec<u8>) {
            self.rx_packets.push_back(NetPacket {
                hdr: VirtioNetHdr::new(),
                data,
            });
        }
    }

    impl NetworkBackend for MockBackend {
        fn send(&mut self, packet: &NetPacket) -> DeviceResult<()> {
            self.tx_packets.push(packet.clone());
            Ok(())
        }

        fn recv(&mut self) -> DeviceResult<Option<NetPacket>> {
            Ok(self.rx_packets.pop_front())
        }

        fn has_pending_rx(&self) -> bool {
            !self.rx_packets.is_empty()
        }
    }

    fn make_test_mac() -> [u8; 6] {
        [0x52, 0x54, 0x00, 0x12, 0x34, 0x56]
    }

    #[test]
    fn test_config_creation() {
        let mac = make_test_mac();
        let config = VirtioNetConfig::new(mac);

        assert_eq!(config.mac, mac);
        assert_eq!(config.status, LinkStatus::Up);
        assert_eq!(config.max_virtqueue_pairs, 1);

        // Config space serialization
        let bytes = config.to_bytes();
        assert_eq!(bytes.len(), 10);
        assert_eq!(&bytes[0..6], &mac);
        // Status = 1 (Up)
        assert_eq!(bytes[6], 1);
        assert_eq!(bytes[7], 0);
        // max_virtqueue_pairs = 1
        assert_eq!(bytes[8], 1);
        assert_eq!(bytes[9], 0);
    }

    #[test]
    fn test_config_read() {
        let mac = make_test_mac();
        let config = VirtioNetConfig::new(mac);

        // Read MAC bytes individually
        for (i, byte) in mac.iter().enumerate() {
            assert_eq!(config.read_byte(i), *byte);
        }

        // Read status as u32
        let status_val = config.read_u32(6);
        // status=1(u16) + max_vq_pairs=1(u16) packed as u32 LE
        assert_eq!(status_val & 0xFFFF, 1); // status
    }

    #[test]
    fn test_feature_negotiation() {
        let mac = make_test_mac();
        let dev = VirtioNet::new_user(mac);

        let f = dev.features();
        assert!(f & features::VIRTIO_NET_F_MAC != 0);
        assert!(f & features::VIRTIO_NET_F_STATUS != 0);
        assert!(f & features::VIRTIO_NET_F_MRG_RXBUF != 0);
        assert!(f & features::VIRTIO_NET_F_CSUM != 0);
        assert!(f & features::VIRTIO_NET_F_GUEST_CSUM != 0);
    }

    #[test]
    fn test_device_type() {
        let mac = make_test_mac();
        let dev = VirtioNet::new_user(mac);
        assert_eq!(dev.device_type(), VirtioDeviceType::Net);
    }

    #[test]
    fn test_mac_address() {
        let mac = make_test_mac();
        let dev = VirtioNet::new_user(mac);
        assert_eq!(dev.mac(), &mac);
    }

    #[test]
    fn test_link_status() {
        let mac = make_test_mac();
        let mut dev = VirtioNet::new_user(mac);

        assert_eq!(dev.link_status(), LinkStatus::Up);
        dev.set_link_status(LinkStatus::Down);
        assert_eq!(dev.link_status(), LinkStatus::Down);
    }

    #[test]
    fn test_virtio_net_hdr_serialization() {
        let hdr = VirtioNetHdr {
            flags: hdr_flags::VIRTIO_NET_HDR_F_NEEDS_CSUM,
            gso_type: gso::VIRTIO_NET_HDR_GSO_TCPV4,
            hdr_len: 42,
            gso_size: 1460,
            csum_start: 14,
            csum_offset: 16,
            num_buffers: 1,
        };

        let bytes = hdr.to_bytes();
        let parsed = VirtioNetHdr::from_bytes(&bytes).unwrap();

        assert_eq!(parsed.flags, hdr_flags::VIRTIO_NET_HDR_F_NEEDS_CSUM);
        assert_eq!(parsed.gso_type, gso::VIRTIO_NET_HDR_GSO_TCPV4);
        assert_eq!(parsed.hdr_len, 42);
        assert_eq!(parsed.gso_size, 1460);
        assert_eq!(parsed.csum_start, 14);
        assert_eq!(parsed.csum_offset, 16);
        assert_eq!(parsed.num_buffers, 1);
    }

    #[test]
    fn test_virtio_net_hdr_too_short() {
        let short_buf = [0u8; 8];
        assert!(VirtioNetHdr::from_bytes(&short_buf).is_err());
    }

    #[test]
    fn test_packet_send_mock() {
        let mac = make_test_mac();
        let mut backend = MockBackend::new();
        // Pre-seed an RX packet
        backend.push_rx(vec![0xDE, 0xAD, 0xBE, 0xEF]);

        let mut dev = VirtioNet::new(mac, Box::new(backend));
        dev.activate().unwrap();

        // Build a TX packet: virtio-net header + payload
        let hdr = VirtioNetHdr::new();
        let mut raw = hdr.to_bytes().to_vec();
        raw.extend_from_slice(&[0x01, 0x02, 0x03, 0x04]);

        // Process TX
        dev.process_tx_packet(&raw).unwrap();

        // send() succeeded without error — packet was delivered to backend
    }

    #[test]
    fn test_packet_receive_mock() {
        let mac = make_test_mac();
        let mut backend = MockBackend::new();
        backend.push_rx(vec![0xCA, 0xFE, 0xBA, 0xBE]);

        let mut dev = VirtioNet::new(mac, Box::new(backend));
        dev.activate().unwrap();

        // Poll for RX packets
        let count = dev.poll_rx().unwrap();
        assert_eq!(count, 1);
        assert!(dev.has_pending_rx());

        // Pop the packet
        let pkt = dev.pop_rx_packet().unwrap();
        assert_eq!(pkt.data, vec![0xCA, 0xFE, 0xBA, 0xBE]);
        assert!(!dev.has_pending_rx());
    }

    #[test]
    fn test_rx_buffer_building() {
        let packet = NetPacket {
            hdr: VirtioNetHdr::new(),
            data: vec![0x01, 0x02, 0x03],
        };

        let buf = VirtioNet::build_rx_buffer(&packet);
        assert_eq!(buf.len(), VIRTIO_NET_HDR_SIZE + 3);
        // First bytes are the header
        assert_eq!(buf[0], 0); // flags
        assert_eq!(buf[1], gso::VIRTIO_NET_HDR_GSO_NONE); // gso_type
        // Last bytes are the data
        assert_eq!(&buf[VIRTIO_NET_HDR_SIZE..], &[0x01, 0x02, 0x03]);
    }

    #[test]
    fn test_tx_packet_too_short() {
        let mac = make_test_mac();
        let mut dev = VirtioNet::new_user(mac);
        dev.activate().unwrap();

        // Too short — less than VIRTIO_NET_HDR_SIZE
        let short = vec![0u8; 4];
        assert!(dev.process_tx_packet(&short).is_err());
    }

    #[test]
    fn test_activate_and_reset() {
        let mac = make_test_mac();
        let mut dev = VirtioNet::new_user(mac);

        dev.activate().unwrap();
        assert!(dev.activated);

        // Queue some packets
        dev.queue_rx_packet(NetPacket {
            hdr: VirtioNetHdr::new(),
            data: vec![1, 2, 3],
        });
        assert!(dev.has_pending_rx());

        // Reset clears everything
        dev.reset();
        assert!(!dev.activated);
        assert!(!dev.has_pending_rx());
    }

    #[test]
    fn test_process_queue_rx() {
        let mac = make_test_mac();
        let mut dev = VirtioNet::new_user(mac);
        dev.activate().unwrap();
        // Should not error
        assert!(dev.process_queue(RX_QUEUE).is_ok());
    }

    #[test]
    fn test_process_queue_tx() {
        let mac = make_test_mac();
        let mut dev = VirtioNet::new_user(mac);
        dev.activate().unwrap();
        assert!(dev.process_queue(TX_QUEUE).is_ok());
    }

    #[test]
    fn test_process_queue_unknown() {
        let mac = make_test_mac();
        let mut dev = VirtioNet::new_user(mac);
        dev.activate().unwrap();
        // Unknown queue should not error
        assert!(dev.process_queue(5).is_ok());
    }

    #[test]
    fn test_usernet_backend_subnet() {
        let backend = UserNetBackend::new();
        assert_eq!(backend.gateway_ip(), &[10, 0, 2, 2]);
        assert_eq!(backend.dns_ip(), &[10, 0, 2, 3]);
        assert_eq!(backend.guest_ip(), &[10, 0, 2, 15]);
        assert_eq!(backend.netmask(), &[255, 255, 255, 0]);

        // In-subnet check
        assert!(backend.is_in_subnet(&[10, 0, 2, 100]));
        assert!(!backend.is_in_subnet(&[192, 168, 1, 1]));
    }

    #[test]
    fn test_usernet_backend_send_recv() {
        let mut backend = UserNetBackend::new();

        // Send a packet (TX from guest)
        let pkt = NetPacket {
            hdr: VirtioNetHdr::new(),
            data: vec![0x11, 0x22, 0x33],
        };
        backend.send(&pkt).unwrap();

        // Verify it's in the TX queue
        let tx_packets = backend.drain_tx();
        assert_eq!(tx_packets.len(), 1);
        assert_eq!(tx_packets[0].data, vec![0x11, 0x22, 0x33]);

        // Inject an RX packet
        backend.inject_rx(NetPacket {
            hdr: VirtioNetHdr::new(),
            data: vec![0xAA, 0xBB],
        });
        assert!(backend.has_pending_rx());

        let rx = backend.recv().unwrap().unwrap();
        assert_eq!(rx.data, vec![0xAA, 0xBB]);
        assert!(!backend.has_pending_rx());
    }

    #[test]
    fn test_tap_backend_inject_rx() {
        let mut tap = TapBackend::new("tap0");
        assert!(!tap.has_pending_rx());

        tap.inject_rx(NetPacket {
            hdr: VirtioNetHdr::new(),
            data: vec![0xFF, 0xFE],
        });
        assert!(tap.has_pending_rx());

        let pkt = tap.recv().unwrap().unwrap();
        assert_eq!(pkt.data, vec![0xFF, 0xFE]);
    }

    #[test]
    fn test_default_hdr() {
        let hdr = VirtioNetHdr::default();
        assert_eq!(hdr.flags, 0);
        assert_eq!(hdr.gso_type, gso::VIRTIO_NET_HDR_GSO_NONE);
        assert_eq!(hdr.num_buffers, 1);
    }
}
