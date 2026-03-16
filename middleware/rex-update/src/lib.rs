//! RexPlayer self-update mechanism
//!
//! Checks for new versions via GitHub releases API and provides
//! download/install functionality for updating the application.

use thiserror::Error;
use serde::{Deserialize, Serialize};

#[derive(Error, Debug)]
pub enum UpdateError {
    #[error("Version check failed: {0}")]
    VersionCheck(String),

    #[error("Download failed: {0}")]
    Download(String),

    #[error("Install failed: {0}")]
    Install(String),

    #[error("Already up to date")]
    AlreadyUpToDate,

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
}

pub type UpdateResult<T> = Result<T, UpdateError>;

/// Semantic version
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
}

impl Version {
    pub fn new(major: u32, minor: u32, patch: u32) -> Self {
        Self { major, minor, patch }
    }

    pub fn parse(s: &str) -> Option<Self> {
        let s = s.strip_prefix('v').unwrap_or(s);
        let parts: Vec<&str> = s.split('.').collect();
        if parts.len() != 3 {
            return None;
        }
        Some(Self {
            major: parts[0].parse().ok()?,
            minor: parts[1].parse().ok()?,
            patch: parts[2].parse().ok()?,
        })
    }

    pub fn is_newer_than(&self, other: &Version) -> bool {
        (self.major, self.minor, self.patch) > (other.major, other.minor, other.patch)
    }
}

impl std::fmt::Display for Version {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)
    }
}

/// Target platform for download
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Platform {
    LinuxX86_64,
    LinuxArm64,
    MacosX86_64,
    MacosArm64,
    WindowsX86_64,
}

impl Platform {
    /// Detect the current platform
    pub fn current() -> Self {
        #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
        { Platform::LinuxX86_64 }
        #[cfg(all(target_os = "linux", target_arch = "aarch64"))]
        { Platform::LinuxArm64 }
        #[cfg(all(target_os = "macos", target_arch = "x86_64"))]
        { Platform::MacosX86_64 }
        #[cfg(all(target_os = "macos", target_arch = "aarch64"))]
        { Platform::MacosArm64 }
        #[cfg(all(target_os = "windows", target_arch = "x86_64"))]
        { Platform::WindowsX86_64 }
        #[cfg(not(any(
            all(target_os = "linux", target_arch = "x86_64"),
            all(target_os = "linux", target_arch = "aarch64"),
            all(target_os = "macos", target_arch = "x86_64"),
            all(target_os = "macos", target_arch = "aarch64"),
            all(target_os = "windows", target_arch = "x86_64"),
        )))]
        { Platform::LinuxX86_64 } // fallback
    }

    pub fn asset_suffix(&self) -> &'static str {
        match self {
            Platform::LinuxX86_64 => "linux-x86_64.AppImage",
            Platform::LinuxArm64 => "linux-arm64.AppImage",
            Platform::MacosX86_64 => "macos-x86_64.dmg",
            Platform::MacosArm64 => "macos-arm64.dmg",
            Platform::WindowsX86_64 => "windows-x86_64.msi",
        }
    }
}

/// Information about an available update
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UpdateInfo {
    pub current_version: Version,
    pub latest_version: Version,
    pub download_url: String,
    pub changelog: String,
    pub asset_size: u64,
}

impl UpdateInfo {
    pub fn has_update(&self) -> bool {
        self.latest_version.is_newer_than(&self.current_version)
    }
}

/// Update checker and installer
pub struct UpdateManager {
    current_version: Version,
    platform: Platform,
    check_url: String,
    last_check: Option<UpdateInfo>,
    auto_check: bool,
}

impl UpdateManager {
    pub fn new(current_version: Version) -> Self {
        Self {
            current_version,
            platform: Platform::current(),
            check_url: "https://api.github.com/repos/rexplayer/rexplayer/releases/latest".into(),
            last_check: None,
            auto_check: true,
        }
    }

    pub fn current_version(&self) -> &Version {
        &self.current_version
    }

    pub fn platform(&self) -> Platform {
        self.platform
    }

    pub fn set_auto_check(&mut self, enabled: bool) {
        self.auto_check = enabled;
    }

    pub fn auto_check(&self) -> bool {
        self.auto_check
    }

    /// Set the last check result (simulates an API call result)
    pub fn set_check_result(&mut self, info: UpdateInfo) {
        self.last_check = Some(info);
    }

    /// Get the last check result
    pub fn last_check(&self) -> Option<&UpdateInfo> {
        self.last_check.as_ref()
    }

    /// Check if an update is available (from cached result)
    pub fn update_available(&self) -> bool {
        self.last_check.as_ref().is_some_and(|info| info.has_update())
    }

    /// Build the expected asset download URL
    pub fn asset_url(&self, version: &Version) -> String {
        format!(
            "https://github.com/rexplayer/rexplayer/releases/download/v{}/rexplayer-{}",
            version,
            self.platform.asset_suffix()
        )
    }

    /// Get the API URL for checking updates
    pub fn check_url(&self) -> &str {
        &self.check_url
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version_parse() {
        let v = Version::parse("1.2.3").unwrap();
        assert_eq!(v, Version::new(1, 2, 3));

        let v = Version::parse("v0.1.0").unwrap();
        assert_eq!(v, Version::new(0, 1, 0));

        assert!(Version::parse("invalid").is_none());
        assert!(Version::parse("1.2").is_none());
    }

    #[test]
    fn test_version_comparison() {
        let v1 = Version::new(1, 0, 0);
        let v2 = Version::new(1, 0, 1);
        let v3 = Version::new(2, 0, 0);

        assert!(v2.is_newer_than(&v1));
        assert!(v3.is_newer_than(&v2));
        assert!(!v1.is_newer_than(&v2));
        assert!(!v1.is_newer_than(&v1));
    }

    #[test]
    fn test_version_display() {
        let v = Version::new(0, 1, 0);
        assert_eq!(v.to_string(), "0.1.0");
    }

    #[test]
    fn test_update_manager_creation() {
        let mgr = UpdateManager::new(Version::new(0, 1, 0));
        assert_eq!(mgr.current_version(), &Version::new(0, 1, 0));
        assert!(mgr.auto_check());
        assert!(!mgr.update_available());
    }

    #[test]
    fn test_update_available() {
        let mut mgr = UpdateManager::new(Version::new(0, 1, 0));

        mgr.set_check_result(UpdateInfo {
            current_version: Version::new(0, 1, 0),
            latest_version: Version::new(0, 2, 0),
            download_url: "https://example.com/update".into(),
            changelog: "New features".into(),
            asset_size: 50_000_000,
        });

        assert!(mgr.update_available());
        let info = mgr.last_check().unwrap();
        assert!(info.has_update());
    }

    #[test]
    fn test_no_update() {
        let mut mgr = UpdateManager::new(Version::new(1, 0, 0));

        mgr.set_check_result(UpdateInfo {
            current_version: Version::new(1, 0, 0),
            latest_version: Version::new(1, 0, 0),
            download_url: String::new(),
            changelog: String::new(),
            asset_size: 0,
        });

        assert!(!mgr.update_available());
    }

    #[test]
    fn test_asset_url() {
        let mgr = UpdateManager::new(Version::new(0, 1, 0));
        let url = mgr.asset_url(&Version::new(0, 2, 0));
        assert!(url.contains("v0.2.0"));
        assert!(url.contains("rexplayer"));
    }

    #[test]
    fn test_platform_asset_suffix() {
        assert!(Platform::LinuxX86_64.asset_suffix().contains("AppImage"));
        assert!(Platform::MacosArm64.asset_suffix().contains("dmg"));
        assert!(Platform::WindowsX86_64.asset_suffix().contains("msi"));
    }
}
