use thiserror::Error;

#[derive(Error, Debug)]
pub enum FridaError {
    #[error("Failed to download Frida: {0}")]
    Download(String),

    #[error("Frida server not running")]
    NotRunning,

    #[error("Connection failed: {0}")]
    Connection(String),
}

/// Manages the Frida server lifecycle
pub struct FridaManager {
    version: Option<String>,
    vsock_port: u32,
}

impl FridaManager {
    pub fn new() -> Self {
        Self {
            version: None,
            vsock_port: 27042,
        }
    }

    /// Get the current Frida server version (if installed)
    pub fn version(&self) -> Option<&str> {
        self.version.as_deref()
    }

    /// Get the vsock port for Frida connections
    pub fn vsock_port(&self) -> u32 {
        self.vsock_port
    }
}
