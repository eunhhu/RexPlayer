//! DNS relay — forwards guest DNS queries to host resolver
//!
//! Minimal DNS packet parsing to relay queries from the guest's
//! virtual DNS server (10.0.2.3) to the host's actual resolver.

use std::net::Ipv4Addr;

/// DNS header flags
pub mod flags {
    pub const QR_QUERY: u16    = 0;
    pub const QR_RESPONSE: u16 = 1 << 15;
    pub const OPCODE_QUERY: u16 = 0;
    pub const RCODE_OK: u16     = 0;
    pub const RCODE_NXDOMAIN: u16 = 3;
    pub const RD: u16 = 1 << 8;  // Recursion Desired
    pub const RA: u16 = 1 << 7;  // Recursion Available
}

/// DNS record types
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DnsRecordType {
    A     = 1,
    AAAA  = 28,
    CNAME = 5,
    MX    = 15,
    NS    = 2,
    PTR   = 12,
    TXT   = 16,
    SOA   = 6,
}

/// Parsed DNS question
#[derive(Debug, Clone)]
pub struct DnsQuestion {
    pub name: String,
    pub qtype: u16,
    pub qclass: u16,
}

/// Parsed DNS header
#[derive(Debug, Clone)]
pub struct DnsHeader {
    pub id: u16,
    pub flags: u16,
    pub qdcount: u16,
    pub ancount: u16,
    pub nscount: u16,
    pub arcount: u16,
}

impl DnsHeader {
    /// Parse from bytes (12 bytes)
    pub fn from_bytes(buf: &[u8]) -> Option<Self> {
        if buf.len() < 12 {
            return None;
        }
        Some(Self {
            id:      u16::from_be_bytes([buf[0], buf[1]]),
            flags:   u16::from_be_bytes([buf[2], buf[3]]),
            qdcount: u16::from_be_bytes([buf[4], buf[5]]),
            ancount: u16::from_be_bytes([buf[6], buf[7]]),
            nscount: u16::from_be_bytes([buf[8], buf[9]]),
            arcount: u16::from_be_bytes([buf[10], buf[11]]),
        })
    }

    /// Serialize to bytes
    pub fn to_bytes(&self) -> [u8; 12] {
        let mut buf = [0u8; 12];
        buf[0..2].copy_from_slice(&self.id.to_be_bytes());
        buf[2..4].copy_from_slice(&self.flags.to_be_bytes());
        buf[4..6].copy_from_slice(&self.qdcount.to_be_bytes());
        buf[6..8].copy_from_slice(&self.ancount.to_be_bytes());
        buf[8..10].copy_from_slice(&self.nscount.to_be_bytes());
        buf[10..12].copy_from_slice(&self.arcount.to_be_bytes());
        buf
    }

    pub fn is_query(&self) -> bool {
        self.flags & flags::QR_RESPONSE == 0
    }
}

/// Parse a DNS name from the question section
/// Returns (name, bytes consumed)
pub fn parse_dns_name(buf: &[u8], offset: usize) -> Option<(String, usize)> {
    let mut name = String::new();
    let mut pos = offset;

    loop {
        if pos >= buf.len() {
            return None;
        }

        let len = buf[pos] as usize;
        if len == 0 {
            pos += 1;
            break;
        }

        // Compression pointer
        if len & 0xC0 == 0xC0 {
            // Not handling compression for relay (just forward raw packet)
            return None;
        }

        pos += 1;
        if pos + len > buf.len() {
            return None;
        }

        if !name.is_empty() {
            name.push('.');
        }
        name.push_str(&String::from_utf8_lossy(&buf[pos..pos + len]));
        pos += len;
    }

    Some((name, pos - offset))
}

/// DNS relay that forwards queries to a host resolver
pub struct DnsRelay {
    /// The virtual DNS address presented to the guest
    pub virtual_dns: Ipv4Addr,
    /// Host resolver addresses
    pub upstream_dns: Vec<Ipv4Addr>,
}

impl DnsRelay {
    pub fn new(virtual_dns: Ipv4Addr) -> Self {
        Self {
            virtual_dns,
            // Default to well-known public DNS
            upstream_dns: vec![
                Ipv4Addr::new(8, 8, 8, 8),
                Ipv4Addr::new(1, 1, 1, 1),
            ],
        }
    }

    /// Check if a packet is a DNS query destined for our virtual DNS
    pub fn is_dns_query(&self, packet: &[u8]) -> bool {
        if packet.len() < 12 {
            return false;
        }
        if let Some(hdr) = DnsHeader::from_bytes(packet) {
            hdr.is_query() && hdr.qdcount > 0
        } else {
            false
        }
    }

    /// Parse the query name from a DNS packet
    pub fn query_name(&self, packet: &[u8]) -> Option<String> {
        if packet.len() < 13 {
            return None;
        }
        parse_dns_name(packet, 12).map(|(name, _)| name)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_dns_query(name: &str) -> Vec<u8> {
        let mut pkt = Vec::new();

        // Header
        let hdr = DnsHeader {
            id: 0x1234,
            flags: flags::RD,
            qdcount: 1,
            ancount: 0,
            nscount: 0,
            arcount: 0,
        };
        pkt.extend_from_slice(&hdr.to_bytes());

        // Question: encode name
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

    #[test]
    fn test_dns_header_roundtrip() {
        let hdr = DnsHeader {
            id: 0xABCD,
            flags: flags::QR_RESPONSE | flags::RA,
            qdcount: 1,
            ancount: 2,
            nscount: 0,
            arcount: 0,
        };

        let bytes = hdr.to_bytes();
        let parsed = DnsHeader::from_bytes(&bytes).unwrap();

        assert_eq!(parsed.id, 0xABCD);
        assert!(!parsed.is_query());
        assert_eq!(parsed.qdcount, 1);
        assert_eq!(parsed.ancount, 2);
    }

    #[test]
    fn test_parse_dns_name() {
        let pkt = make_dns_query("example.com");
        let (name, _) = parse_dns_name(&pkt, 12).unwrap();
        assert_eq!(name, "example.com");
    }

    #[test]
    fn test_dns_relay_is_query() {
        let relay = DnsRelay::new(Ipv4Addr::new(10, 0, 2, 3));
        let pkt = make_dns_query("google.com");
        assert!(relay.is_dns_query(&pkt));

        // Not a query (too short)
        assert!(!relay.is_dns_query(&[0u8; 4]));
    }

    #[test]
    fn test_dns_relay_query_name() {
        let relay = DnsRelay::new(Ipv4Addr::new(10, 0, 2, 3));
        let pkt = make_dns_query("test.example.org");
        let name = relay.query_name(&pkt).unwrap();
        assert_eq!(name, "test.example.org");
    }

    #[test]
    fn test_dns_header_too_short() {
        assert!(DnsHeader::from_bytes(&[0u8; 6]).is_none());
    }
}
