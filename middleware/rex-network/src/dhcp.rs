//! Minimal DHCP server for guest network configuration
//!
//! Implements DHCP DISCOVER → OFFER → REQUEST → ACK flow.

use std::collections::HashMap;
use std::net::Ipv4Addr;

use crate::NatConfig;

/// DHCP message types
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DhcpMessageType {
    Discover = 1,
    Offer    = 2,
    Request  = 3,
    Decline  = 4,
    Ack      = 5,
    Nak      = 6,
    Release  = 7,
    Inform   = 8,
}

impl DhcpMessageType {
    pub fn from_u8(val: u8) -> Option<Self> {
        match val {
            1 => Some(Self::Discover),
            2 => Some(Self::Offer),
            3 => Some(Self::Request),
            4 => Some(Self::Decline),
            5 => Some(Self::Ack),
            6 => Some(Self::Nak),
            7 => Some(Self::Release),
            8 => Some(Self::Inform),
            _ => None,
        }
    }
}

/// DHCP lease entry
#[derive(Debug, Clone)]
pub struct DhcpLease {
    pub mac: [u8; 6],
    pub ip: Ipv4Addr,
    pub lease_time: u32,
}

/// Minimal DHCP server
pub struct DhcpServer {
    config: NatConfig,
    leases: HashMap<[u8; 6], DhcpLease>,
    next_ip: u32,
}

impl DhcpServer {
    pub fn new(config: NatConfig) -> Self {
        let next_ip = u32::from(config.dhcp_start);
        Self {
            config,
            leases: HashMap::new(),
            next_ip,
        }
    }

    /// Allocate an IP for the given MAC, or return existing lease
    pub fn allocate(&mut self, mac: [u8; 6]) -> Option<DhcpLease> {
        // Return existing lease if any
        if let Some(lease) = self.leases.get(&mac) {
            return Some(lease.clone());
        }

        // Allocate new IP
        let end_ip = u32::from(self.config.dhcp_end);
        if self.next_ip > end_ip {
            return None; // Pool exhausted
        }

        let ip = Ipv4Addr::from(self.next_ip);
        self.next_ip += 1;

        let lease = DhcpLease {
            mac,
            ip,
            lease_time: 86400, // 24 hours
        };
        self.leases.insert(mac, lease.clone());
        Some(lease)
    }

    /// Release a lease by MAC
    pub fn release(&mut self, mac: &[u8; 6]) {
        self.leases.remove(mac);
    }

    /// Get the server's gateway address
    pub fn gateway(&self) -> Ipv4Addr {
        self.config.gateway
    }

    /// Get the DNS server address
    pub fn dns(&self) -> Ipv4Addr {
        self.config.dns
    }

    /// Get the subnet mask
    pub fn netmask(&self) -> Ipv4Addr {
        self.config.netmask
    }

    /// Get lease count
    pub fn lease_count(&self) -> usize {
        self.leases.len()
    }

    /// Process a DHCP message and generate a response
    ///
    /// Returns (response_type, allocated_ip) or None if invalid
    pub fn process_message(
        &mut self,
        msg_type: DhcpMessageType,
        client_mac: [u8; 6],
    ) -> Option<(DhcpMessageType, DhcpLease)> {
        match msg_type {
            DhcpMessageType::Discover => {
                let lease = self.allocate(client_mac)?;
                Some((DhcpMessageType::Offer, lease))
            }
            DhcpMessageType::Request => {
                let lease = self.allocate(client_mac)?;
                Some((DhcpMessageType::Ack, lease))
            }
            DhcpMessageType::Release => {
                self.release(&client_mac);
                None
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_mac() -> [u8; 6] {
        [0x52, 0x54, 0x00, 0x12, 0x34, 0x56]
    }

    #[test]
    fn test_dhcp_discover_offer() {
        let config = NatConfig::default();
        let mut server = DhcpServer::new(config);

        let result = server.process_message(DhcpMessageType::Discover, test_mac());
        assert!(result.is_some());

        let (msg_type, lease) = result.unwrap();
        assert_eq!(msg_type, DhcpMessageType::Offer);
        assert_eq!(lease.ip, Ipv4Addr::new(10, 0, 2, 15));
        assert_eq!(lease.mac, test_mac());
    }

    #[test]
    fn test_dhcp_request_ack() {
        let config = NatConfig::default();
        let mut server = DhcpServer::new(config);

        // Discover first
        server.process_message(DhcpMessageType::Discover, test_mac());

        // Request should return ACK with same IP
        let result = server.process_message(DhcpMessageType::Request, test_mac());
        let (msg_type, lease) = result.unwrap();
        assert_eq!(msg_type, DhcpMessageType::Ack);
        assert_eq!(lease.ip, Ipv4Addr::new(10, 0, 2, 15));
    }

    #[test]
    fn test_dhcp_multiple_clients() {
        let config = NatConfig::default();
        let mut server = DhcpServer::new(config);

        let mac1 = [0x52, 0x54, 0x00, 0x00, 0x00, 0x01];
        let mac2 = [0x52, 0x54, 0x00, 0x00, 0x00, 0x02];

        let (_, lease1) = server.process_message(DhcpMessageType::Discover, mac1).unwrap();
        let (_, lease2) = server.process_message(DhcpMessageType::Discover, mac2).unwrap();

        assert_eq!(lease1.ip, Ipv4Addr::new(10, 0, 2, 15));
        assert_eq!(lease2.ip, Ipv4Addr::new(10, 0, 2, 16));
        assert_eq!(server.lease_count(), 2);
    }

    #[test]
    fn test_dhcp_same_mac_same_ip() {
        let config = NatConfig::default();
        let mut server = DhcpServer::new(config);

        let (_, l1) = server.process_message(DhcpMessageType::Discover, test_mac()).unwrap();
        let (_, l2) = server.process_message(DhcpMessageType::Discover, test_mac()).unwrap();

        assert_eq!(l1.ip, l2.ip);
        assert_eq!(server.lease_count(), 1);
    }

    #[test]
    fn test_dhcp_release() {
        let config = NatConfig::default();
        let mut server = DhcpServer::new(config);

        server.process_message(DhcpMessageType::Discover, test_mac());
        assert_eq!(server.lease_count(), 1);

        server.process_message(DhcpMessageType::Release, test_mac());
        assert_eq!(server.lease_count(), 0);
    }

    #[test]
    fn test_dhcp_server_info() {
        let config = NatConfig::default();
        let server = DhcpServer::new(config);

        assert_eq!(server.gateway(), Ipv4Addr::new(10, 0, 2, 2));
        assert_eq!(server.dns(), Ipv4Addr::new(10, 0, 2, 3));
        assert_eq!(server.netmask(), Ipv4Addr::new(255, 255, 255, 0));
    }

    #[test]
    fn test_message_type_from_u8() {
        assert_eq!(DhcpMessageType::from_u8(1), Some(DhcpMessageType::Discover));
        assert_eq!(DhcpMessageType::from_u8(5), Some(DhcpMessageType::Ack));
        assert_eq!(DhcpMessageType::from_u8(99), None);
    }
}
