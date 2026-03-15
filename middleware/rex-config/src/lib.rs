//! Configuration management (TOML-based)

use serde::{Deserialize, Serialize};

/// Top-level RexPlayer configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RexConfig {
    pub vm: VmConfig,
    pub display: DisplayConfig,
    pub frida: FridaConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VmConfig {
    #[serde(default = "default_vcpus")]
    pub vcpus: u32,
    #[serde(default = "default_ram")]
    pub ram_mb: u64,
    pub system_image: String,
    pub data_image: Option<String>,
    pub kernel: Option<String>,
    pub initrd: Option<String>,
    #[serde(default)]
    pub kernel_cmdline: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DisplayConfig {
    #[serde(default = "default_width")]
    pub width: u32,
    #[serde(default = "default_height")]
    pub height: u32,
    #[serde(default = "default_dpi")]
    pub dpi: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FridaConfig {
    #[serde(default = "default_true")]
    pub enabled: bool,
    #[serde(default = "default_true")]
    pub auto_update: bool,
    #[serde(default = "default_frida_port")]
    pub port: u16,
}

fn default_vcpus() -> u32 { 2 }
fn default_ram() -> u64 { 2048 }
fn default_width() -> u32 { 1080 }
fn default_height() -> u32 { 1920 }
fn default_dpi() -> u32 { 320 }
fn default_true() -> bool { true }
fn default_frida_port() -> u16 { 27042 }

impl Default for RexConfig {
    fn default() -> Self {
        Self {
            vm: VmConfig {
                vcpus: default_vcpus(),
                ram_mb: default_ram(),
                system_image: "system.img".into(),
                data_image: None,
                kernel: None,
                initrd: None,
                kernel_cmdline: String::new(),
            },
            display: DisplayConfig {
                width: default_width(),
                height: default_height(),
                dpi: default_dpi(),
            },
            frida: FridaConfig {
                enabled: true,
                auto_update: true,
                port: default_frida_port(),
            },
        }
    }
}

impl RexConfig {
    /// Load configuration from a TOML file
    pub fn load(path: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let content = std::fs::read_to_string(path)?;
        let config: Self = toml::from_str(&content)?;
        Ok(config)
    }

    /// Save configuration to a TOML file
    pub fn save(&self, path: &str) -> Result<(), Box<dyn std::error::Error>> {
        let content = toml::to_string_pretty(self)?;
        std::fs::write(path, content)?;
        Ok(())
    }

    /// Parse configuration from a TOML string
    pub fn from_toml(s: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let config: Self = toml::from_str(s)?;
        Ok(config)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_config() {
        let config = RexConfig::default();
        assert_eq!(config.vm.vcpus, 2);
        assert_eq!(config.vm.ram_mb, 2048);
        assert_eq!(config.display.width, 1080);
        assert_eq!(config.display.height, 1920);
        assert_eq!(config.display.dpi, 320);
        assert!(config.frida.enabled);
        assert_eq!(config.frida.port, 27042);
    }

    #[test]
    fn test_serialize_deserialize() {
        let config = RexConfig::default();
        let toml_str = toml::to_string_pretty(&config).unwrap();
        let parsed = RexConfig::from_toml(&toml_str).unwrap();
        assert_eq!(parsed.vm.vcpus, config.vm.vcpus);
        assert_eq!(parsed.vm.ram_mb, config.vm.ram_mb);
        assert_eq!(parsed.display.width, config.display.width);
        assert_eq!(parsed.frida.port, config.frida.port);
    }

    #[test]
    fn test_parse_minimal_toml() {
        let toml_str = r#"
[vm]
system_image = "android.img"

[display]

[frida]
"#;
        let config = RexConfig::from_toml(toml_str).unwrap();
        assert_eq!(config.vm.system_image, "android.img");
        assert_eq!(config.vm.vcpus, 2); // default
        assert_eq!(config.vm.ram_mb, 2048); // default
        assert!(config.frida.enabled); // default
    }

    #[test]
    fn test_parse_custom_values() {
        let toml_str = r#"
[vm]
vcpus = 4
ram_mb = 4096
system_image = "system.img"
kernel = "bzImage"
kernel_cmdline = "console=ttyS0"

[display]
width = 1440
height = 2560
dpi = 560

[frida]
enabled = false
auto_update = false
port = 12345
"#;
        let config = RexConfig::from_toml(toml_str).unwrap();
        assert_eq!(config.vm.vcpus, 4);
        assert_eq!(config.vm.ram_mb, 4096);
        assert_eq!(config.vm.kernel.as_deref(), Some("bzImage"));
        assert_eq!(config.vm.kernel_cmdline, "console=ttyS0");
        assert_eq!(config.display.width, 1440);
        assert_eq!(config.display.height, 2560);
        assert_eq!(config.display.dpi, 560);
        assert!(!config.frida.enabled);
        assert!(!config.frida.auto_update);
        assert_eq!(config.frida.port, 12345);
    }

    #[test]
    fn test_save_and_load() {
        let config = RexConfig::default();
        let tmp = std::env::temp_dir().join("rex_config_test.toml");
        let path = tmp.to_str().unwrap();

        config.save(path).unwrap();
        let loaded = RexConfig::load(path).unwrap();
        assert_eq!(loaded.vm.vcpus, config.vm.vcpus);
        assert_eq!(loaded.vm.ram_mb, config.vm.ram_mb);

        std::fs::remove_file(path).ok();
    }
}
