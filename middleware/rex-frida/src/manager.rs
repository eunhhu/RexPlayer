//! Frida Server lifecycle management
//!
//! Handles downloading, installing, and connecting to Frida Server
//! running inside the Android guest via vsock.

use thiserror::Error;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Error, Debug)]
pub enum FridaError {
    #[error("Failed to download Frida: {0}")]
    Download(String),

    #[error("Frida server not running")]
    NotRunning,

    #[error("Connection failed: {0}")]
    Connection(String),

    #[error("Version check failed: {0}")]
    VersionCheck(String),

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Invalid architecture: {0}")]
    InvalidArch(String),
}

pub type FridaResult<T> = Result<T, FridaError>;

/// Target architecture for Frida server binary
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FridaArch {
    X86_64,
    Arm64,
    X86,
    Arm,
}

impl FridaArch {
    pub fn as_str(&self) -> &'static str {
        match self {
            FridaArch::X86_64 => "x86_64",
            FridaArch::Arm64 => "arm64",
            FridaArch::X86 => "x86",
            FridaArch::Arm => "arm",
        }
    }

    pub fn from_str(s: &str) -> FridaResult<Self> {
        match s {
            "x86_64" | "amd64" => Ok(FridaArch::X86_64),
            "arm64" | "aarch64" => Ok(FridaArch::Arm64),
            "x86" | "i686" => Ok(FridaArch::X86),
            "arm" | "armhf" => Ok(FridaArch::Arm),
            _ => Err(FridaError::InvalidArch(s.to_string())),
        }
    }

    /// GitHub release asset name for this architecture
    pub fn asset_name(&self, version: &str) -> String {
        format!("frida-server-{}-android-{}.xz", version, self.as_str())
    }
}

/// Frida server connection state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FridaState {
    /// Not installed
    NotInstalled,
    /// Installed but not running
    Stopped,
    /// Running and accepting connections
    Running,
    /// Connected to a client
    Connected,
}

/// Vsock bridge configuration
#[derive(Debug, Clone)]
pub struct VsockBridgeConfig {
    /// Guest CID
    pub guest_cid: u64,
    /// Frida port on guest (vsock)
    pub guest_port: u32,
    /// Local TCP port to bridge to
    pub host_port: u16,
}

impl Default for VsockBridgeConfig {
    fn default() -> Self {
        Self {
            guest_cid: 3,
            guest_port: 27042,
            host_port: 27042,
        }
    }
}

/// Manages the Frida server lifecycle
pub struct FridaManager {
    /// Current installed version
    version: Option<String>,
    /// Latest known version from GitHub
    latest_version: Option<String>,
    /// Target architecture
    arch: FridaArch,
    /// Installation directory
    install_dir: PathBuf,
    /// Server binary path
    server_path: Option<PathBuf>,
    /// Current state
    state: FridaState,
    /// Vsock bridge config
    bridge_config: VsockBridgeConfig,
    /// Auto-update enabled
    auto_update: bool,
}

impl FridaManager {
    /// Create a new FridaManager
    pub fn new(arch: FridaArch, install_dir: PathBuf) -> Self {
        Self {
            version: None,
            latest_version: None,
            arch,
            install_dir,
            server_path: None,
            state: FridaState::NotInstalled,
            bridge_config: VsockBridgeConfig::default(),
            auto_update: true,
        }
    }

    /// Create with default paths
    pub fn with_defaults() -> Self {
        let install_dir = dirs_default();
        Self::new(FridaArch::X86_64, install_dir)
    }

    /// Get the current Frida server version (if installed)
    pub fn version(&self) -> Option<&str> {
        self.version.as_deref()
    }

    /// Get the latest known version
    pub fn latest_version(&self) -> Option<&str> {
        self.latest_version.as_deref()
    }

    /// Get the target architecture
    pub fn arch(&self) -> FridaArch {
        self.arch
    }

    /// Get the current state
    pub fn state(&self) -> FridaState {
        self.state
    }

    /// Get the vsock port for Frida connections
    pub fn vsock_port(&self) -> u32 {
        self.bridge_config.guest_port
    }

    /// Get the host TCP port
    pub fn host_port(&self) -> u16 {
        self.bridge_config.host_port
    }

    /// Set the bridge configuration
    pub fn set_bridge_config(&mut self, config: VsockBridgeConfig) {
        self.bridge_config = config;
    }

    /// Set auto-update
    pub fn set_auto_update(&mut self, enabled: bool) {
        self.auto_update = enabled;
    }

    /// Check if an update is available
    pub fn update_available(&self) -> bool {
        match (&self.version, &self.latest_version) {
            (Some(current), Some(latest)) => current != latest,
            (None, Some(_)) => true,
            _ => false,
        }
    }

    /// Get the expected server binary path
    pub fn server_binary_path(&self) -> PathBuf {
        self.install_dir.join(format!("frida-server-{}", self.arch.as_str()))
    }

    /// Check if Frida server is installed locally
    pub fn is_installed(&self) -> bool {
        self.server_path.is_some() || self.server_binary_path().exists()
    }

    /// Set the installed version (after download/detection)
    pub fn set_installed(&mut self, version: String, path: PathBuf) {
        self.version = Some(version);
        self.server_path = Some(path);
        self.state = FridaState::Stopped;
    }

    /// Mark as running
    pub fn set_running(&mut self) {
        self.state = FridaState::Running;
    }

    /// Mark as stopped
    pub fn set_stopped(&mut self) {
        self.state = FridaState::Stopped;
    }

    /// Mark as connected
    pub fn set_connected(&mut self) {
        self.state = FridaState::Connected;
    }

    /// Set the latest version (after GitHub API check)
    pub fn set_latest_version(&mut self, version: String) {
        self.latest_version = Some(version);
    }

    /// Build the GitHub release download URL
    pub fn download_url(&self, version: &str) -> String {
        format!(
            "https://github.com/frida/frida/releases/download/{}/{}",
            version,
            self.arch.asset_name(version)
        )
    }

    /// Build the GitHub API URL for latest release
    pub fn latest_release_api_url() -> &'static str {
        "https://api.github.com/repos/frida/frida/releases/latest"
    }

    /// Get version file path (stored alongside binary)
    pub fn version_file_path(&self) -> PathBuf {
        self.install_dir.join("frida-version.txt")
    }

    /// Read installed version from version file
    pub fn read_version_file(&mut self) -> FridaResult<Option<String>> {
        let path = self.version_file_path();
        if !path.exists() {
            return Ok(None);
        }
        let version = std::fs::read_to_string(&path)?.trim().to_string();
        if !version.is_empty() {
            self.version = Some(version.clone());
            self.state = FridaState::Stopped;
            Ok(Some(version))
        } else {
            Ok(None)
        }
    }

    /// Write version to version file
    pub fn write_version_file(&self, version: &str) -> FridaResult<()> {
        std::fs::create_dir_all(&self.install_dir)?;
        std::fs::write(self.version_file_path(), version)?;
        Ok(())
    }

    /// Get status summary
    pub fn status_summary(&self) -> HashMap<String, String> {
        let mut info = HashMap::new();
        info.insert("state".into(), format!("{:?}", self.state));
        info.insert("arch".into(), self.arch.as_str().into());
        info.insert("vsock_port".into(), self.bridge_config.guest_port.to_string());
        info.insert("host_port".into(), self.bridge_config.host_port.to_string());
        info.insert("auto_update".into(), self.auto_update.to_string());
        if let Some(v) = &self.version {
            info.insert("version".into(), v.clone());
        }
        if let Some(v) = &self.latest_version {
            info.insert("latest_version".into(), v.clone());
        }
        info
    }
}

fn dirs_default() -> PathBuf {
    #[cfg(target_os = "linux")]
    {
        PathBuf::from(std::env::var("HOME").unwrap_or_default())
            .join(".local/share/rexplayer/frida")
    }
    #[cfg(target_os = "macos")]
    {
        PathBuf::from(std::env::var("HOME").unwrap_or_default())
            .join("Library/Application Support/RexPlayer/frida")
    }
    #[cfg(target_os = "windows")]
    {
        PathBuf::from(std::env::var("APPDATA").unwrap_or_default())
            .join("RexPlayer/frida")
    }
    #[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
    {
        PathBuf::from("/tmp/rexplayer/frida")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_frida_arch() {
        assert_eq!(FridaArch::X86_64.as_str(), "x86_64");
        assert_eq!(FridaArch::Arm64.as_str(), "arm64");
        assert_eq!(FridaArch::from_str("aarch64").unwrap(), FridaArch::Arm64);
        assert_eq!(FridaArch::from_str("amd64").unwrap(), FridaArch::X86_64);
        assert!(FridaArch::from_str("mips").is_err());
    }

    #[test]
    fn test_asset_name() {
        let name = FridaArch::X86_64.asset_name("16.2.1");
        assert_eq!(name, "frida-server-16.2.1-android-x86_64.xz");
    }

    #[test]
    fn test_download_url() {
        let mgr = FridaManager::new(FridaArch::Arm64, PathBuf::from("/tmp/frida"));
        let url = mgr.download_url("16.2.1");
        assert!(url.contains("github.com/frida/frida/releases"));
        assert!(url.contains("arm64"));
    }

    #[test]
    fn test_creation() {
        let mgr = FridaManager::new(FridaArch::X86_64, PathBuf::from("/tmp/frida"));
        assert_eq!(mgr.state(), FridaState::NotInstalled);
        assert_eq!(mgr.arch(), FridaArch::X86_64);
        assert_eq!(mgr.vsock_port(), 27042);
        assert_eq!(mgr.host_port(), 27042);
        assert!(mgr.version().is_none());
    }

    #[test]
    fn test_lifecycle() {
        let mut mgr = FridaManager::new(FridaArch::X86_64, PathBuf::from("/tmp/frida"));
        assert_eq!(mgr.state(), FridaState::NotInstalled);

        mgr.set_installed("16.2.1".into(), PathBuf::from("/tmp/frida/frida-server"));
        assert_eq!(mgr.state(), FridaState::Stopped);
        assert_eq!(mgr.version(), Some("16.2.1"));

        mgr.set_running();
        assert_eq!(mgr.state(), FridaState::Running);

        mgr.set_connected();
        assert_eq!(mgr.state(), FridaState::Connected);

        mgr.set_stopped();
        assert_eq!(mgr.state(), FridaState::Stopped);
    }

    #[test]
    fn test_update_available() {
        let mut mgr = FridaManager::new(FridaArch::X86_64, PathBuf::from("/tmp/frida"));

        // No versions known
        assert!(!mgr.update_available());

        // Only latest known
        mgr.set_latest_version("16.3.0".into());
        assert!(mgr.update_available());

        // Same version
        mgr.set_installed("16.3.0".into(), PathBuf::from("/tmp"));
        assert!(!mgr.update_available());

        // Different versions
        mgr.set_latest_version("16.4.0".into());
        assert!(mgr.update_available());
    }

    #[test]
    fn test_bridge_config() {
        let mut mgr = FridaManager::new(FridaArch::X86_64, PathBuf::from("/tmp/frida"));
        mgr.set_bridge_config(VsockBridgeConfig {
            guest_cid: 5,
            guest_port: 12345,
            host_port: 54321,
        });
        assert_eq!(mgr.vsock_port(), 12345);
        assert_eq!(mgr.host_port(), 54321);
    }

    #[test]
    fn test_status_summary() {
        let mut mgr = FridaManager::new(FridaArch::Arm64, PathBuf::from("/tmp/frida"));
        mgr.set_installed("16.2.1".into(), PathBuf::from("/tmp"));

        let summary = mgr.status_summary();
        assert_eq!(summary.get("arch").unwrap(), "arm64");
        assert_eq!(summary.get("version").unwrap(), "16.2.1");
        assert_eq!(summary.get("state").unwrap(), "Stopped");
    }

    #[test]
    fn test_version_file() {
        let tmp = std::env::temp_dir().join("rex_frida_test");
        let mut mgr = FridaManager::new(FridaArch::X86_64, tmp.clone());

        mgr.write_version_file("16.2.1").unwrap();
        let ver = mgr.read_version_file().unwrap();
        assert_eq!(ver, Some("16.2.1".to_string()));

        std::fs::remove_dir_all(&tmp).ok();
    }

    #[test]
    fn test_server_binary_path() {
        let mgr = FridaManager::new(FridaArch::Arm64, PathBuf::from("/opt/frida"));
        let path = mgr.server_binary_path();
        assert_eq!(path, PathBuf::from("/opt/frida/frida-server-arm64"));
    }
}
