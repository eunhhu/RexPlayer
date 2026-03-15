//! Virtio vsock device backend
//!
//! Implements the virtio-vsock device per virtio specification (Section 5.10).
//! Provides AF_VSOCK host-guest communication used for ADB and Frida.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::{HashMap, VecDeque};

// ============================================================================
// Constants
// ============================================================================

/// Host CID (always 2 per vsock spec)
pub const HOST_CID: u64 = 2;

/// Virtqueue indices
pub const RX_QUEUE: u16 = 0;
pub const TX_QUEUE: u16 = 1;
pub const EVENT_QUEUE: u16 = 2;

/// Well-known guest ports
pub const ADB_PORT: u32 = 5555;
pub const FRIDA_PORT: u32 = 27042;

// ============================================================================
// Vsock packet header (virtio spec 5.10.2)
// ============================================================================

/// Size of the vsock packet header
pub const VSOCK_HDR_SIZE: usize = 44;

/// Vsock operations
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VsockOp {
    Invalid   = 0,
    Request   = 1,  // Connection request
    Response  = 2,  // Connection response
    Rst       = 3,  // Reset (connection refused / closed)
    Shutdown  = 4,  // Graceful shutdown
    Rw        = 5,  // Data transfer
    CreditUpdate = 6, // Credit update
    CreditRequest = 7, // Credit request
}

impl VsockOp {
    pub fn from_u16(val: u16) -> Self {
        match val {
            1 => VsockOp::Request,
            2 => VsockOp::Response,
            3 => VsockOp::Rst,
            4 => VsockOp::Shutdown,
            5 => VsockOp::Rw,
            6 => VsockOp::CreditUpdate,
            7 => VsockOp::CreditRequest,
            _ => VsockOp::Invalid,
        }
    }
}

/// Vsock packet types
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VsockType {
    Stream = 1,
}

/// Shutdown flags
pub mod shutdown_flags {
    pub const VIRTIO_VSOCK_SHUTDOWN_RCV: u32 = 1;
    pub const VIRTIO_VSOCK_SHUTDOWN_SEND: u32 = 2;
}

/// Vsock packet header
#[derive(Debug, Clone)]
pub struct VsockPacket {
    pub src_cid: u64,
    pub dst_cid: u64,
    pub src_port: u32,
    pub dst_port: u32,
    pub len: u32,
    pub pkt_type: u16,
    pub op: VsockOp,
    pub flags: u32,
    pub buf_alloc: u32,
    pub fwd_cnt: u32,
    pub data: Vec<u8>,
}

impl VsockPacket {
    /// Create a new empty packet
    pub fn new(op: VsockOp) -> Self {
        Self {
            src_cid: 0,
            dst_cid: 0,
            src_port: 0,
            dst_port: 0,
            len: 0,
            pkt_type: VsockType::Stream as u16,
            op,
            flags: 0,
            buf_alloc: 0,
            fwd_cnt: 0,
            data: Vec::new(),
        }
    }

    /// Create a response packet for a given request
    pub fn reply(req: &VsockPacket, op: VsockOp) -> Self {
        Self {
            src_cid: req.dst_cid,
            dst_cid: req.src_cid,
            src_port: req.dst_port,
            dst_port: req.src_port,
            len: 0,
            pkt_type: VsockType::Stream as u16,
            op,
            flags: 0,
            buf_alloc: 65536,
            fwd_cnt: 0,
            data: Vec::new(),
        }
    }

    /// Serialize header to bytes (little-endian, 44 bytes)
    pub fn header_to_bytes(&self) -> [u8; VSOCK_HDR_SIZE] {
        let mut buf = [0u8; VSOCK_HDR_SIZE];
        buf[0..8].copy_from_slice(&self.src_cid.to_le_bytes());
        buf[8..16].copy_from_slice(&self.dst_cid.to_le_bytes());
        buf[16..20].copy_from_slice(&self.src_port.to_le_bytes());
        buf[20..24].copy_from_slice(&self.dst_port.to_le_bytes());
        buf[24..28].copy_from_slice(&self.len.to_le_bytes());
        buf[28..30].copy_from_slice(&self.pkt_type.to_le_bytes());
        buf[30..32].copy_from_slice(&(self.op as u16).to_le_bytes());
        buf[32..36].copy_from_slice(&self.flags.to_le_bytes());
        buf[36..40].copy_from_slice(&self.buf_alloc.to_le_bytes());
        buf[40..44].copy_from_slice(&self.fwd_cnt.to_le_bytes());
        buf
    }

    /// Parse header from bytes
    pub fn from_header_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < VSOCK_HDR_SIZE {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            src_cid: u64::from_le_bytes(buf[0..8].try_into().unwrap()),
            dst_cid: u64::from_le_bytes(buf[8..16].try_into().unwrap()),
            src_port: u32::from_le_bytes(buf[16..20].try_into().unwrap()),
            dst_port: u32::from_le_bytes(buf[20..24].try_into().unwrap()),
            len: u32::from_le_bytes(buf[24..28].try_into().unwrap()),
            pkt_type: u16::from_le_bytes(buf[28..30].try_into().unwrap()),
            op: VsockOp::from_u16(u16::from_le_bytes(buf[30..32].try_into().unwrap())),
            flags: u32::from_le_bytes(buf[32..36].try_into().unwrap()),
            buf_alloc: u32::from_le_bytes(buf[36..40].try_into().unwrap()),
            fwd_cnt: u32::from_le_bytes(buf[40..44].try_into().unwrap()),
            data: Vec::new(),
        })
    }
}

// ============================================================================
// Connection state machine
// ============================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnState {
    Listen,
    SynSent,
    Established,
    Closing,
    Closed,
}

/// Connection key (local_port, peer_port, peer_cid)
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ConnKey {
    pub local_port: u32,
    pub peer_port: u32,
    pub peer_cid: u64,
}

/// A single vsock connection
#[derive(Debug)]
pub struct VsockConnection {
    pub key: ConnKey,
    pub state: ConnState,
    pub buf_alloc: u32,
    pub fwd_cnt: u32,
    pub peer_buf_alloc: u32,
    pub peer_fwd_cnt: u32,
    pub tx_cnt: u32,
    pub rx_buf: VecDeque<u8>,
}

impl VsockConnection {
    pub fn new(key: ConnKey) -> Self {
        Self {
            key,
            state: ConnState::Listen,
            buf_alloc: 65536,
            fwd_cnt: 0,
            peer_buf_alloc: 0,
            peer_fwd_cnt: 0,
            tx_cnt: 0,
            rx_buf: VecDeque::with_capacity(65536),
        }
    }

    /// Available space the peer can send to us
    pub fn credit_available(&self) -> u32 {
        self.buf_alloc.saturating_sub(self.fwd_cnt.wrapping_sub(self.peer_fwd_cnt))
    }
}

// ============================================================================
// VirtioVsock device
// ============================================================================

/// Host-side listener callback for incoming guest connections
pub type ListenerCallback = Box<dyn Fn(&VsockPacket) -> bool + Send>;

pub struct VirtioVsock {
    /// Guest CID
    guest_cid: u64,
    features: u64,
    activated: bool,
    /// Active connections
    connections: HashMap<ConnKey, VsockConnection>,
    /// Packets to deliver to the guest (RX)
    rx_queue: VecDeque<VsockPacket>,
    /// Host-side port listeners
    listeners: HashMap<u32, Vec<u32>>,
}

impl VirtioVsock {
    pub fn new(guest_cid: u64) -> Self {
        Self {
            guest_cid,
            features: 0,
            activated: false,
            connections: HashMap::new(),
            rx_queue: VecDeque::with_capacity(64),
            listeners: HashMap::new(),
        }
    }

    pub fn guest_cid(&self) -> u64 {
        self.guest_cid
    }

    /// Register a host-side listener on a port
    pub fn listen(&mut self, port: u32) {
        self.listeners.entry(port).or_default();
    }

    /// Process a packet from the guest (TX path)
    pub fn process_guest_packet(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        match pkt.op {
            VsockOp::Request => self.handle_request(pkt),
            VsockOp::Response => self.handle_response(pkt),
            VsockOp::Rw => self.handle_rw(pkt),
            VsockOp::Shutdown => self.handle_shutdown(pkt),
            VsockOp::Rst => self.handle_rst(pkt),
            VsockOp::CreditUpdate => self.handle_credit_update(pkt),
            VsockOp::CreditRequest => self.handle_credit_request(pkt),
            _ => Ok(()),
        }
    }

    fn conn_key_from_guest(pkt: &VsockPacket) -> ConnKey {
        ConnKey {
            local_port: pkt.dst_port,
            peer_port: pkt.src_port,
            peer_cid: pkt.src_cid,
        }
    }

    fn handle_request(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);

        // Check if we're listening on this port
        if !self.listeners.contains_key(&pkt.dst_port) {
            // Send RST — connection refused
            let mut rst = VsockPacket::reply(pkt, VsockOp::Rst);
            rst.src_cid = HOST_CID;
            self.rx_queue.push_back(rst);
            return Ok(());
        }

        // Accept the connection
        let mut conn = VsockConnection::new(key);
        conn.state = ConnState::Established;
        conn.peer_buf_alloc = pkt.buf_alloc;
        conn.peer_fwd_cnt = pkt.fwd_cnt;
        self.connections.insert(key, conn);

        // Send RESPONSE
        let mut resp = VsockPacket::reply(pkt, VsockOp::Response);
        resp.src_cid = HOST_CID;
        resp.buf_alloc = 65536;
        self.rx_queue.push_back(resp);

        Ok(())
    }

    fn handle_response(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);
        if let Some(conn) = self.connections.get_mut(&key) {
            if conn.state == ConnState::SynSent {
                conn.state = ConnState::Established;
                conn.peer_buf_alloc = pkt.buf_alloc;
                conn.peer_fwd_cnt = pkt.fwd_cnt;
            }
        }
        Ok(())
    }

    fn handle_rw(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);
        if let Some(conn) = self.connections.get_mut(&key) {
            if conn.state == ConnState::Established {
                conn.rx_buf.extend(&pkt.data);
                conn.fwd_cnt = conn.fwd_cnt.wrapping_add(pkt.len);
            }
        }
        Ok(())
    }

    fn handle_shutdown(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);
        if let Some(conn) = self.connections.get_mut(&key) {
            conn.state = ConnState::Closing;

            // Send RST to acknowledge
            let mut rst = VsockPacket::reply(pkt, VsockOp::Rst);
            rst.src_cid = HOST_CID;
            self.rx_queue.push_back(rst);

            conn.state = ConnState::Closed;
        }
        Ok(())
    }

    fn handle_rst(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);
        self.connections.remove(&key);
        Ok(())
    }

    fn handle_credit_update(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);
        if let Some(conn) = self.connections.get_mut(&key) {
            conn.peer_buf_alloc = pkt.buf_alloc;
            conn.peer_fwd_cnt = pkt.fwd_cnt;
        }
        Ok(())
    }

    fn handle_credit_request(&mut self, pkt: &VsockPacket) -> DeviceResult<()> {
        let key = Self::conn_key_from_guest(pkt);
        if let Some(conn) = self.connections.get(&key) {
            let mut update = VsockPacket::reply(pkt, VsockOp::CreditUpdate);
            update.src_cid = HOST_CID;
            update.buf_alloc = conn.buf_alloc;
            update.fwd_cnt = conn.fwd_cnt;
            self.rx_queue.push_back(update);
        }
        Ok(())
    }

    /// Send data from the host to the guest on an established connection
    pub fn send_to_guest(&mut self, key: &ConnKey, data: &[u8]) -> DeviceResult<()> {
        if let Some(conn) = self.connections.get_mut(key) {
            if conn.state != ConnState::Established {
                return Err(DeviceError::NotReady);
            }

            let mut pkt = VsockPacket::new(VsockOp::Rw);
            pkt.src_cid = HOST_CID;
            pkt.dst_cid = self.guest_cid;
            pkt.src_port = key.local_port;
            pkt.dst_port = key.peer_port;
            pkt.len = data.len() as u32;
            pkt.buf_alloc = conn.buf_alloc;
            pkt.fwd_cnt = conn.fwd_cnt;
            pkt.data = data.to_vec();

            conn.tx_cnt = conn.tx_cnt.wrapping_add(data.len() as u32);
            self.rx_queue.push_back(pkt);
            Ok(())
        } else {
            Err(DeviceError::NotReady)
        }
    }

    /// Pop a packet for delivery to the guest
    pub fn pop_rx_packet(&mut self) -> Option<VsockPacket> {
        self.rx_queue.pop_front()
    }

    /// Check if there are pending RX packets
    pub fn has_pending_rx(&self) -> bool {
        !self.rx_queue.is_empty()
    }

    /// Get a connection by key
    pub fn get_connection(&self, key: &ConnKey) -> Option<&VsockConnection> {
        self.connections.get(key)
    }

    /// Read data from a connection's receive buffer
    pub fn read_conn_data(&mut self, key: &ConnKey, buf: &mut [u8]) -> usize {
        if let Some(conn) = self.connections.get_mut(key) {
            let n = buf.len().min(conn.rx_buf.len());
            for (i, byte) in conn.rx_buf.drain(..n).enumerate() {
                buf[i] = byte;
            }
            n
        } else {
            0
        }
    }
}

impl VirtioDevice for VirtioVsock {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Vsock
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        // Auto-listen on well-known ports
        self.listen(ADB_PORT);
        self.listen(FRIDA_PORT);
        tracing::info!("virtio-vsock activated: CID {}, listening on ports {}, {}",
            self.guest_cid, ADB_PORT, FRIDA_PORT);
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        self.connections.clear();
        self.rx_queue.clear();
        self.listeners.clear();
    }

    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
        match queue_index {
            RX_QUEUE => {
                tracing::trace!("virtio-vsock: RX queue notified");
                Ok(())
            }
            TX_QUEUE => {
                tracing::trace!("virtio-vsock: TX queue notified");
                Ok(())
            }
            EVENT_QUEUE => {
                tracing::trace!("virtio-vsock: event queue notified");
                Ok(())
            }
            _ => Ok(()),
        }
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_request(guest_cid: u64, src_port: u32, dst_port: u32) -> VsockPacket {
        VsockPacket {
            src_cid: guest_cid,
            dst_cid: HOST_CID,
            src_port,
            dst_port,
            len: 0,
            pkt_type: VsockType::Stream as u16,
            op: VsockOp::Request,
            flags: 0,
            buf_alloc: 65536,
            fwd_cnt: 0,
            data: Vec::new(),
        }
    }

    #[test]
    fn test_creation() {
        let dev = VirtioVsock::new(3);
        assert_eq!(dev.guest_cid(), 3);
        assert_eq!(dev.device_type(), VirtioDeviceType::Vsock);
    }

    #[test]
    fn test_activate_auto_listen() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();
        assert!(dev.listeners.contains_key(&ADB_PORT));
        assert!(dev.listeners.contains_key(&FRIDA_PORT));
    }

    #[test]
    fn test_connection_request_accepted() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Guest connects to host ADB port
        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();

        // Should get a RESPONSE
        let resp = dev.pop_rx_packet().unwrap();
        assert_eq!(resp.op, VsockOp::Response);
        assert_eq!(resp.dst_cid, 3);
        assert_eq!(resp.dst_port, 1234);

        // Connection should be established
        let key = ConnKey { local_port: ADB_PORT, peer_port: 1234, peer_cid: 3 };
        let conn = dev.get_connection(&key).unwrap();
        assert_eq!(conn.state, ConnState::Established);
    }

    #[test]
    fn test_connection_request_refused() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Guest connects to port we're not listening on
        let req = make_request(3, 1234, 9999);
        dev.process_guest_packet(&req).unwrap();

        // Should get RST
        let rst = dev.pop_rx_packet().unwrap();
        assert_eq!(rst.op, VsockOp::Rst);
    }

    #[test]
    fn test_data_transfer_guest_to_host() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Establish connection
        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();
        dev.pop_rx_packet(); // consume RESPONSE

        // Guest sends data
        let mut rw = VsockPacket::new(VsockOp::Rw);
        rw.src_cid = 3;
        rw.dst_cid = HOST_CID;
        rw.src_port = 1234;
        rw.dst_port = ADB_PORT;
        rw.len = 5;
        rw.data = vec![b'h', b'e', b'l', b'l', b'o'];
        dev.process_guest_packet(&rw).unwrap();

        // Read data from connection
        let key = ConnKey { local_port: ADB_PORT, peer_port: 1234, peer_cid: 3 };
        let mut buf = [0u8; 16];
        let n = dev.read_conn_data(&key, &mut buf);
        assert_eq!(n, 5);
        assert_eq!(&buf[..5], b"hello");
    }

    #[test]
    fn test_data_transfer_host_to_guest() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Establish connection
        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();
        dev.pop_rx_packet(); // consume RESPONSE

        // Host sends data to guest
        let key = ConnKey { local_port: ADB_PORT, peer_port: 1234, peer_cid: 3 };
        dev.send_to_guest(&key, b"world").unwrap();

        // Should have RW packet queued
        let pkt = dev.pop_rx_packet().unwrap();
        assert_eq!(pkt.op, VsockOp::Rw);
        assert_eq!(pkt.data, b"world");
        assert_eq!(pkt.dst_cid, 3);
    }

    #[test]
    fn test_shutdown() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Establish
        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();
        dev.pop_rx_packet();

        // Shutdown
        let mut shutdown = VsockPacket::new(VsockOp::Shutdown);
        shutdown.src_cid = 3;
        shutdown.dst_cid = HOST_CID;
        shutdown.src_port = 1234;
        shutdown.dst_port = ADB_PORT;
        shutdown.flags = shutdown_flags::VIRTIO_VSOCK_SHUTDOWN_RCV
            | shutdown_flags::VIRTIO_VSOCK_SHUTDOWN_SEND;
        dev.process_guest_packet(&shutdown).unwrap();

        // Should get RST
        let rst = dev.pop_rx_packet().unwrap();
        assert_eq!(rst.op, VsockOp::Rst);

        // Connection should be closed
        let key = ConnKey { local_port: ADB_PORT, peer_port: 1234, peer_cid: 3 };
        let conn = dev.get_connection(&key).unwrap();
        assert_eq!(conn.state, ConnState::Closed);
    }

    #[test]
    fn test_rst_removes_connection() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Establish
        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();
        dev.pop_rx_packet();

        // RST from guest
        let mut rst = VsockPacket::new(VsockOp::Rst);
        rst.src_cid = 3;
        rst.dst_cid = HOST_CID;
        rst.src_port = 1234;
        rst.dst_port = ADB_PORT;
        dev.process_guest_packet(&rst).unwrap();

        let key = ConnKey { local_port: ADB_PORT, peer_port: 1234, peer_cid: 3 };
        assert!(dev.get_connection(&key).is_none());
    }

    #[test]
    fn test_packet_serialization() {
        let pkt = VsockPacket {
            src_cid: 3,
            dst_cid: HOST_CID,
            src_port: 1234,
            dst_port: 5555,
            len: 10,
            pkt_type: VsockType::Stream as u16,
            op: VsockOp::Rw,
            flags: 0,
            buf_alloc: 65536,
            fwd_cnt: 100,
            data: vec![],
        };

        let bytes = pkt.header_to_bytes();
        assert_eq!(bytes.len(), VSOCK_HDR_SIZE);

        let parsed = VsockPacket::from_header_bytes(&bytes).unwrap();
        assert_eq!(parsed.src_cid, 3);
        assert_eq!(parsed.dst_cid, HOST_CID);
        assert_eq!(parsed.src_port, 1234);
        assert_eq!(parsed.dst_port, 5555);
        assert_eq!(parsed.len, 10);
        assert_eq!(parsed.op, VsockOp::Rw);
        assert_eq!(parsed.buf_alloc, 65536);
        assert_eq!(parsed.fwd_cnt, 100);
    }

    #[test]
    fn test_packet_header_too_short() {
        let short = [0u8; 20];
        assert!(VsockPacket::from_header_bytes(&short).is_err());
    }

    #[test]
    fn test_credit_request() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        // Establish
        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();
        dev.pop_rx_packet();

        // Credit request from guest
        let mut credit_req = VsockPacket::new(VsockOp::CreditRequest);
        credit_req.src_cid = 3;
        credit_req.dst_cid = HOST_CID;
        credit_req.src_port = 1234;
        credit_req.dst_port = ADB_PORT;
        dev.process_guest_packet(&credit_req).unwrap();

        // Should get credit update
        let update = dev.pop_rx_packet().unwrap();
        assert_eq!(update.op, VsockOp::CreditUpdate);
        assert_eq!(update.buf_alloc, 65536);
    }

    #[test]
    fn test_reset_clears_all() {
        let mut dev = VirtioVsock::new(3);
        dev.activate().unwrap();

        let req = make_request(3, 1234, ADB_PORT);
        dev.process_guest_packet(&req).unwrap();

        dev.reset();
        assert!(dev.connections.is_empty());
        assert!(dev.rx_queue.is_empty());
        assert!(dev.listeners.is_empty());
    }

    #[test]
    fn test_reply_packet() {
        let req = make_request(3, 1234, ADB_PORT);
        let resp = VsockPacket::reply(&req, VsockOp::Response);

        assert_eq!(resp.src_cid, HOST_CID);
        assert_eq!(resp.dst_cid, 3);
        assert_eq!(resp.src_port, ADB_PORT);
        assert_eq!(resp.dst_port, 1234);
        assert_eq!(resp.op, VsockOp::Response);
    }

    #[test]
    fn test_vsock_op_from_u16() {
        assert_eq!(VsockOp::from_u16(1), VsockOp::Request);
        assert_eq!(VsockOp::from_u16(5), VsockOp::Rw);
        assert_eq!(VsockOp::from_u16(99), VsockOp::Invalid);
    }
}
