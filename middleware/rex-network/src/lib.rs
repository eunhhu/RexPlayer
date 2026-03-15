//! Network middleware — NAT, DHCP server, DNS relay
//!
//! Provides userspace networking for the guest VM without requiring
//! root privileges or TAP device setup.

pub mod dhcp;
pub mod dns;

use std::net::Ipv4Addr;

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
