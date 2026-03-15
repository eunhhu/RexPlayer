//! Network middleware — NAT, DHCP server, DNS relay
//!
//! Provides userspace networking for the guest VM without requiring
//! root privileges or TAP device setup.
//!
//! The `UserNet` struct ties together the DHCP server, DNS relay,
//! and IP address management into a single NAT gateway for the guest.

pub mod dhcp;
pub mod dns;

use std::collections::HashMap;
use std::net::Ipv4Addr;

use dhcp::{DhcpServer, DhcpMessageType};
use dns::DnsRelay;

/// NAT subnet configuration
#[derive(Debug, Clone)]
pub struct NatConfig {
    /// Guest subnet (e.g., 10.0.2.0)
    pub subnet: Ipv4Addr,
    /// Subnet mask (e.g., 255.255.255.0)
    pub netmask: Ipv4Addr,
    /// Gateway address (e.g., 10.0.2.2)
    pub gateway: Ipv4Addr,
    /// DNS server address (e.g., 10.0.2.3)
    pub dns: Ipv4Addr,
    /// DHCP range start
    pub dhcp_start: Ipv4Addr,
    /// DHCP range end
    pub dhcp_end: Ipv4Addr,
}

impl Default for NatConfig {
    fn default() -> Self {
        Self {
            subnet: Ipv4Addr::new(10, 0, 2, 0),
            netmask: Ipv4Addr::new(255, 255, 255, 0),
            gateway: Ipv4Addr::new(10, 0, 2, 2),
            dns: Ipv4Addr::new(10, 0, 2, 3),
            dhcp_start: Ipv4Addr::new(10, 0, 2, 15),
            dhcp_end: Ipv4Addr::new(10, 0, 2, 30),
        }
    }
}

impl NatConfig {
    /// Check if an IP address is within the configured subnet
    pub fn is_in_subnet(&self, ip: &Ipv4Addr) -> bool {
        let ip_bits = u32::from(*ip);
        let subnet_bits = u32::from(self.subnet);
        let mask_bits = u32::from(self.netmask);
        (ip_bits & mask_bits) == (subnet_bits & mask_bits)
    }

    /// Get the broadcast address for the subnet
    pub fn broadcast(&self) -> Ipv4Addr {
        let subnet_bits = u32::from(self.subnet);
        let mask_bits = u32::from(self.netmask);
        Ipv4Addr::from(subnet_bits | !mask_bits)
    }
}

// ============================================================================
// NAT connection tracking
// ============================================================================

/// Protocol identifiers for NAT connection tracking
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NatProtocol {
    Tcp,
    Udp,
    Icmp,
}

/// Key for tracking a NAT translation entry
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NatEntry {
    /// Protocol
    pub protocol: NatProtocol,
    /// Guest (internal) source IP
    pub guest_ip: Ipv4Addr,
    /// Guest source port
    pub guest_port: u16,
    /// Host-side translated port
    pub host_port: u16,
    /// Destination IP (external)
    pub dest_ip: Ipv4Addr,
    /// Destination port
    pub dest_port: u16,
}

/// Port forwarding rule: forwards host_port -> guest_ip:guest_port
#[derive(Debug, Clone)]
pub struct PortForward {
    /// Protocol to forward
    pub protocol: NatProtocol,
    /// Host port to listen on
    pub host_port: u16,
    /// Guest IP to forward to
    pub guest_ip: Ipv4Addr,
    /// Guest port to forward to
    pub guest_port: u16,
}

// ============================================================================
// UserNet: userspace NAT for guest networking
// ============================================================================

/// Userspace NAT gateway combining DHCP, DNS relay, and IP address management.
///
/// Provides complete networking for the guest without root privileges:
/// - DHCP server assigns IPs from the 10.0.2.0/24 subnet
/// - DNS relay forwards DNS queries to the host's resolver
/// - NAT translates guest traffic to host network
/// - Port forwarding allows host-initiated connections to the guest
pub struct UserNet {
    /// NAT configuration
    config: NatConfig,
    /// DHCP server
    dhcp: DhcpServer,
    /// DNS relay
    dns_relay: DnsRelay,
    /// Active NAT translation entries (guest_key -> NatEntry)
    nat_table: HashMap<(NatProtocol, Ipv4Addr, u16, Ipv4Addr, u16), NatEntry>,
    /// Port forwarding rules
    port_forwards: Vec<PortForward>,
    /// Next available ephemeral port for NAT translation
    next_nat_port: u16,
    /// MAC address for the virtual gateway
    gateway_mac: [u8; 6],
}

impl UserNet {
    /// Create a new UserNet with default configuration (10.0.2.0/24 subnet)
    pub fn new() -> Self {
        Self::with_config(NatConfig::default())
    }

    /// Create a new UserNet with custom configuration
    pub fn with_config(config: NatConfig) -> Self {
        let dhcp = DhcpServer::new(config.clone());
        let dns_relay = DnsRelay::new(config.dns);

        Self {
            config,
            dhcp,
            dns_relay,
            nat_table: HashMap::new(),
            port_forwards: Vec::new(),
            next_nat_port: 10000,
            gateway_mac: [0x52, 0x55, 0x00, 0x00, 0x00, 0x01], // virtual gateway MAC
        }
    }

    /// Get the NAT configuration
    pub fn config(&self) -> &NatConfig {
        &self.config
    }

    /// Get a reference to the DHCP server
    pub fn dhcp(&self) -> &DhcpServer {
        &self.dhcp
    }

    /// Get a mutable reference to the DHCP server
    pub fn dhcp_mut(&mut self) -> &mut DhcpServer {
        &mut self.dhcp
    }

    /// Get a reference to the DNS relay
    pub fn dns_relay(&self) -> &DnsRelay {
        &self.dns_relay
    }

    /// Get the gateway IP
    pub fn gateway_ip(&self) -> Ipv4Addr {
        self.config.gateway
    }

    /// Get the DNS IP
    pub fn dns_ip(&self) -> Ipv4Addr {
        self.config.dns
    }

    /// Get the virtual gateway MAC address
    pub fn gateway_mac(&self) -> &[u8; 6] {
        &self.gateway_mac
    }

    /// Get the subnet mask
    pub fn netmask(&self) -> Ipv4Addr {
        self.config.netmask
    }

    /// Process a DHCP message from a guest
    pub fn handle_dhcp(
        &mut self,
        msg_type: DhcpMessageType,
        client_mac: [u8; 6],
    ) -> Option<(DhcpMessageType, dhcp::DhcpLease)> {
        self.dhcp.process_message(msg_type, client_mac)
    }

    /// Check if a DNS packet should be handled by our relay
    pub fn is_dns_query(&self, packet: &[u8]) -> bool {
        self.dns_relay.is_dns_query(packet)
    }

    /// Get the query name from a DNS packet
    pub fn dns_query_name(&self, packet: &[u8]) -> Option<String> {
        self.dns_relay.query_name(packet)
    }

    /// Add a port forwarding rule
    pub fn add_port_forward(&mut self, forward: PortForward) {
        self.port_forwards.push(forward);
    }

    /// Remove a port forwarding rule by host port and protocol
    pub fn remove_port_forward(&mut self, protocol: NatProtocol, host_port: u16) {
        self.port_forwards.retain(|f| {
            !(f.protocol == protocol && f.host_port == host_port)
        });
    }

    /// Look up a port forwarding rule for incoming traffic
    pub fn find_port_forward(
        &self,
        protocol: NatProtocol,
        host_port: u16,
    ) -> Option<&PortForward> {
        self.port_forwards.iter().find(|f| {
            f.protocol == protocol && f.host_port == host_port
        })
    }

    /// Allocate an ephemeral port for NAT translation
    fn alloc_nat_port(&mut self) -> u16 {
        let port = self.next_nat_port;
        self.next_nat_port = if self.next_nat_port >= 60000 {
            10000
        } else {
            self.next_nat_port + 1
        };
        port
    }

    /// Create or look up a NAT translation for outgoing guest traffic
    pub fn translate_outgoing(
        &mut self,
        protocol: NatProtocol,
        guest_ip: Ipv4Addr,
        guest_port: u16,
        dest_ip: Ipv4Addr,
        dest_port: u16,
    ) -> NatEntry {
        let key = (protocol, guest_ip, guest_port, dest_ip, dest_port);

        if let Some(entry) = self.nat_table.get(&key) {
            return *entry;
        }

        let host_port = self.alloc_nat_port();
        let entry = NatEntry {
            protocol,
            guest_ip,
            guest_port,
            host_port,
            dest_ip,
            dest_port,
        };

        self.nat_table.insert(key, entry);
        entry
    }

    /// Look up the NAT entry for incoming traffic (reverse NAT)
    pub fn translate_incoming(
        &self,
        protocol: NatProtocol,
        host_port: u16,
        source_ip: Ipv4Addr,
        source_port: u16,
    ) -> Option<&NatEntry> {
        self.nat_table.values().find(|e| {
            e.protocol == protocol
                && e.host_port == host_port
                && e.dest_ip == source_ip
                && e.dest_port == source_port
        })
    }

    /// Get the number of active NAT entries
    pub fn nat_table_size(&self) -> usize {
        self.nat_table.len()
    }

    /// Clear all NAT translations (e.g., on reset)
    pub fn clear_nat_table(&mut self) {
        self.nat_table.clear();
        self.next_nat_port = 10000;
    }

    /// Check if an IP packet from the guest is destined for
    /// the gateway itself (DHCP, DNS, etc.)
    pub fn is_gateway_traffic(&self, dest_ip: &Ipv4Addr) -> bool {
        *dest_ip == self.config.gateway || *dest_ip == self.config.dns
    }

    /// Check if traffic is a broadcast within the subnet
    pub fn is_broadcast(&self, dest_ip: &Ipv4Addr) -> bool {
        *dest_ip == self.config.broadcast() || *dest_ip == Ipv4Addr::BROADCAST
    }
}

impl Default for UserNet {
    fn default() -> Self {
        Self::new()
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = NatConfig::default();
        assert_eq!(config.subnet, Ipv4Addr::new(10, 0, 2, 0));
        assert_eq!(config.netmask, Ipv4Addr::new(255, 255, 255, 0));
        assert_eq!(config.gateway, Ipv4Addr::new(10, 0, 2, 2));
        assert_eq!(config.dns, Ipv4Addr::new(10, 0, 2, 3));
        assert_eq!(config.dhcp_start, Ipv4Addr::new(10, 0, 2, 15));
        assert_eq!(config.dhcp_end, Ipv4Addr::new(10, 0, 2, 30));
    }

    #[test]
    fn test_subnet_check() {
        let config = NatConfig::default();
        assert!(config.is_in_subnet(&Ipv4Addr::new(10, 0, 2, 15)));
        assert!(config.is_in_subnet(&Ipv4Addr::new(10, 0, 2, 100)));
        assert!(!config.is_in_subnet(&Ipv4Addr::new(192, 168, 1, 1)));
        assert!(!config.is_in_subnet(&Ipv4Addr::new(10, 0, 3, 1)));
    }

    #[test]
    fn test_broadcast_address() {
        let config = NatConfig::default();
        assert_eq!(config.broadcast(), Ipv4Addr::new(10, 0, 2, 255));
    }

    #[test]
    fn test_usernet_creation() {
        let net = UserNet::new();
        assert_eq!(net.gateway_ip(), Ipv4Addr::new(10, 0, 2, 2));
        assert_eq!(net.dns_ip(), Ipv4Addr::new(10, 0, 2, 3));
        assert_eq!(net.netmask(), Ipv4Addr::new(255, 255, 255, 0));
    }

    #[test]
    fn test_usernet_dhcp_flow() {
        let mut net = UserNet::new();
        let mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];

        // DISCOVER -> OFFER
        let result = net.handle_dhcp(DhcpMessageType::Discover, mac);
        assert!(result.is_some());
        let (msg_type, lease) = result.unwrap();
        assert_eq!(msg_type, DhcpMessageType::Offer);
        assert_eq!(lease.ip, Ipv4Addr::new(10, 0, 2, 15));

        // REQUEST -> ACK
        let result = net.handle_dhcp(DhcpMessageType::Request, mac);
        let (msg_type, lease) = result.unwrap();
        assert_eq!(msg_type, DhcpMessageType::Ack);
        assert_eq!(lease.ip, Ipv4Addr::new(10, 0, 2, 15));
    }

    #[test]
    fn test_usernet_dns_query() {
        let net = UserNet::new();

        // Build a minimal DNS query packet
        let query = build_test_dns_query("example.com");
        assert!(net.is_dns_query(&query));

        let name = net.dns_query_name(&query).unwrap();
        assert_eq!(name, "example.com");
    }

    #[test]
    fn test_usernet_dns_not_query() {
        let net = UserNet::new();
        // Too short to be DNS
        assert!(!net.is_dns_query(&[0u8; 4]));
    }

    #[test]
    fn test_nat_translation_outgoing() {
        let mut net = UserNet::new();

        let entry = net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            12345,
            Ipv4Addr::new(93, 184, 216, 34), // example.com
            80,
        );

        assert_eq!(entry.guest_ip, Ipv4Addr::new(10, 0, 2, 15));
        assert_eq!(entry.guest_port, 12345);
        assert_eq!(entry.dest_ip, Ipv4Addr::new(93, 184, 216, 34));
        assert_eq!(entry.dest_port, 80);
        assert!(entry.host_port >= 10000);
        assert_eq!(net.nat_table_size(), 1);
    }

    #[test]
    fn test_nat_translation_same_flow_reuses_entry() {
        let mut net = UserNet::new();

        let e1 = net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            12345,
            Ipv4Addr::new(93, 184, 216, 34),
            80,
        );

        let e2 = net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            12345,
            Ipv4Addr::new(93, 184, 216, 34),
            80,
        );

        assert_eq!(e1.host_port, e2.host_port);
        assert_eq!(net.nat_table_size(), 1);
    }

    #[test]
    fn test_nat_translation_incoming() {
        let mut net = UserNet::new();

        let entry = net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            12345,
            Ipv4Addr::new(93, 184, 216, 34),
            80,
        );

        // Reverse lookup
        let found = net.translate_incoming(
            NatProtocol::Tcp,
            entry.host_port,
            Ipv4Addr::new(93, 184, 216, 34),
            80,
        );
        assert!(found.is_some());
        let found = found.unwrap();
        assert_eq!(found.guest_ip, Ipv4Addr::new(10, 0, 2, 15));
        assert_eq!(found.guest_port, 12345);
    }

    #[test]
    fn test_nat_translation_incoming_not_found() {
        let net = UserNet::new();
        let result = net.translate_incoming(
            NatProtocol::Tcp,
            9999,
            Ipv4Addr::new(1, 2, 3, 4),
            80,
        );
        assert!(result.is_none());
    }

    #[test]
    fn test_port_forwarding() {
        let mut net = UserNet::new();

        net.add_port_forward(PortForward {
            protocol: NatProtocol::Tcp,
            host_port: 5555,
            guest_ip: Ipv4Addr::new(10, 0, 2, 15),
            guest_port: 5555,
        });

        let fwd = net.find_port_forward(NatProtocol::Tcp, 5555);
        assert!(fwd.is_some());
        let fwd = fwd.unwrap();
        assert_eq!(fwd.guest_ip, Ipv4Addr::new(10, 0, 2, 15));
        assert_eq!(fwd.guest_port, 5555);

        // Not found for different protocol
        assert!(net.find_port_forward(NatProtocol::Udp, 5555).is_none());

        // Remove
        net.remove_port_forward(NatProtocol::Tcp, 5555);
        assert!(net.find_port_forward(NatProtocol::Tcp, 5555).is_none());
    }

    #[test]
    fn test_gateway_traffic_detection() {
        let net = UserNet::new();
        assert!(net.is_gateway_traffic(&Ipv4Addr::new(10, 0, 2, 2)));
        assert!(net.is_gateway_traffic(&Ipv4Addr::new(10, 0, 2, 3)));
        assert!(!net.is_gateway_traffic(&Ipv4Addr::new(10, 0, 2, 15)));
    }

    #[test]
    fn test_broadcast_detection() {
        let net = UserNet::new();
        assert!(net.is_broadcast(&Ipv4Addr::new(10, 0, 2, 255)));
        assert!(net.is_broadcast(&Ipv4Addr::new(255, 255, 255, 255)));
        assert!(!net.is_broadcast(&Ipv4Addr::new(10, 0, 2, 15)));
    }

    #[test]
    fn test_clear_nat_table() {
        let mut net = UserNet::new();

        net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            12345,
            Ipv4Addr::new(8, 8, 8, 8),
            53,
        );
        assert_eq!(net.nat_table_size(), 1);

        net.clear_nat_table();
        assert_eq!(net.nat_table_size(), 0);
    }

    #[test]
    fn test_multiple_nat_entries() {
        let mut net = UserNet::new();

        // Two different flows get different ports
        let e1 = net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            1000,
            Ipv4Addr::new(1, 1, 1, 1),
            80,
        );

        let e2 = net.translate_outgoing(
            NatProtocol::Tcp,
            Ipv4Addr::new(10, 0, 2, 15),
            1001,
            Ipv4Addr::new(1, 1, 1, 1),
            80,
        );

        assert_ne!(e1.host_port, e2.host_port);
        assert_eq!(net.nat_table_size(), 2);
    }

    /// Helper to build a minimal DNS query packet for testing
    fn build_test_dns_query(name: &str) -> Vec<u8> {
        let mut pkt = Vec::new();

        // DNS header: ID=0x1234, flags=RD, qdcount=1
        let hdr = dns::DnsHeader {
            id: 0x1234,
            flags: dns::flags::RD,
            qdcount: 1,
            ancount: 0,
            nscount: 0,
            arcount: 0,
        };
        pkt.extend_from_slice(&hdr.to_bytes());

        // Encode domain name
        for label in name.split('.') {
            pkt.push(label.len() as u8);
            pkt.extend_from_slice(label.as_bytes());
        }
        pkt.push(0); // root label

        // QTYPE = A (1), QCLASS = IN (1)
        pkt.extend_from_slice(&1u16.to_be_bytes());
        pkt.extend_from_slice(&1u16.to_be_bytes());

        pkt
    }
}
