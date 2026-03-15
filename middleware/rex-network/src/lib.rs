//! Network middleware — NAT, DNS relay, proxy support

pub struct NatConfig {
    pub subnet: String,
    pub gateway: String,
    pub dns: String,
}

impl Default for NatConfig {
    fn default() -> Self {
        Self {
            subnet: "10.0.2.0/24".into(),
            gateway: "10.0.2.2".into(),
            dns: "10.0.2.3".into(),
        }
    }
}
