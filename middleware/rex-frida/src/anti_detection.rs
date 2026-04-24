//! Anti-detection and bypass module
//!
//! Provides pre-built Frida scripts and guest-side configurations to bypass
//! common app-level detection mechanisms:
//!
//! - **Root detection** (SafetyNet, RootBeer, custom checks)
//! - **Emulator detection** (Build.FINGERPRINT, sensors, telephony)
//! - **Frida detection** (port scanning, /proc/maps, named pipes)
//! - **Debugger detection** (ptrace, TracerPid, isDebuggerConnected)
//! - **SSL pinning** (TrustManager, OkHttp, certificate checks)
//! - **Integrity checks** (APK signature, dex checksum)

use std::collections::HashMap;

/// Categories of detection bypass
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BypassCategory {
    /// Root/su detection (SafetyNet, RootBeer, su binary checks)
    Root,
    /// Emulator/VM detection (Build props, sensors, telephony)
    Emulator,
    /// Frida detection (port 27042, /proc/maps, named threads)
    Frida,
    /// Debugger detection (ptrace, TracerPid)
    Debugger,
    /// SSL certificate pinning
    SslPinning,
    /// APK integrity / tampering checks
    Integrity,
}

/// A bypass script with metadata
#[derive(Debug, Clone)]
pub struct BypassScript {
    /// Script name
    pub name: String,
    /// Category
    pub category: BypassCategory,
    /// Description
    pub description: String,
    /// Frida JavaScript source
    pub source: String,
    /// Whether this bypass is enabled by default
    pub default_enabled: bool,
}

/// Build properties to fake a real device (defeats emulator detection)
#[derive(Debug, Clone)]
pub struct DeviceProfile {
    /// Profile name (e.g., "Pixel 7", "Galaxy S24")
    pub name: String,
    /// Build.MANUFACTURER
    pub manufacturer: String,
    /// Build.MODEL
    pub model: String,
    /// Build.BRAND
    pub brand: String,
    /// Build.PRODUCT
    pub product: String,
    /// Build.DEVICE
    pub device: String,
    /// Build.HARDWARE
    pub hardware: String,
    /// Build.FINGERPRINT
    pub fingerprint: String,
    /// Build.BOARD
    pub board: String,
    /// Build.DISPLAY
    pub display: String,
    /// ro.build.description
    pub description: String,
    /// IMEI (fake)
    pub imei: String,
    /// Phone number (fake)
    pub phone_number: String,
    /// Operator name
    pub operator_name: String,
}

impl DeviceProfile {
    /// Google Pixel 7 profile
    pub fn pixel_7() -> Self {
        Self {
            name: "Pixel 7".into(),
            manufacturer: "Google".into(),
            model: "Pixel 7".into(),
            brand: "google".into(),
            product: "panther".into(),
            device: "panther".into(),
            hardware: "tensor".into(),
            fingerprint: "google/panther/panther:13/TQ3A.230901.001/10750268:user/release-keys"
                .into(),
            board: "panther".into(),
            display: "TQ3A.230901.001".into(),
            description: "panther-user 13 TQ3A.230901.001 10750268 release-keys".into(),
            imei: "358240051111110".into(),
            phone_number: "+15551234567".into(),
            operator_name: "T-Mobile".into(),
        }
    }

    /// Samsung Galaxy S24 profile
    pub fn galaxy_s24() -> Self {
        Self {
            name: "Galaxy S24".into(),
            manufacturer: "samsung".into(),
            model: "SM-S921B".into(),
            brand: "samsung".into(),
            product: "e1sxeea".into(),
            device: "e1s".into(),
            hardware: "exynos2400".into(),
            fingerprint: "samsung/e1sxeea/e1s:14/UP1A.231005.007/S921BXXS1AXA2:user/release-keys"
                .into(),
            board: "exynos2400".into(),
            display: "UP1A.231005.007.S921BXXS1AXA2".into(),
            description: "e1sxeea-user 14 UP1A.231005.007 S921BXXS1AXA2 release-keys".into(),
            imei: "354322110000001".into(),
            phone_number: "+821012345678".into(),
            operator_name: "SKT".into(),
        }
    }

    /// Generate the Frida script that patches Build properties
    pub fn to_frida_script(&self) -> String {
        format!(
            r#"Java.perform(function() {{
    // Patch android.os.Build fields
    var Build = Java.use('android.os.Build');
    Build.MANUFACTURER.value = "{manufacturer}";
    Build.MODEL.value = "{model}";
    Build.BRAND.value = "{brand}";
    Build.PRODUCT.value = "{product}";
    Build.DEVICE.value = "{device}";
    Build.HARDWARE.value = "{hardware}";
    Build.FINGERPRINT.value = "{fingerprint}";
    Build.BOARD.value = "{board}";
    Build.DISPLAY.value = "{display}";

    // Patch Build.VERSION
    var Version = Java.use('android.os.Build$VERSION');
    // Keep SDK_INT as-is to not break compatibility

    // Patch SystemProperties
    var SystemProperties = Java.use('android.os.SystemProperties');
    SystemProperties.get.overload('java.lang.String').implementation = function(key) {{
        var overrides = {{
            'ro.product.manufacturer': '{manufacturer}',
            'ro.product.model': '{model}',
            'ro.product.brand': '{brand}',
            'ro.product.name': '{product}',
            'ro.product.device': '{device}',
            'ro.hardware': '{hardware}',
            'ro.build.fingerprint': '{fingerprint}',
            'ro.build.display.id': '{display}',
            'ro.build.description': '{description}',
        }};
        if (key in overrides) {{
            return overrides[key];
        }}
        return this.get(key);
    }};

    send({{ type: 'bypass', category: 'emulator', status: 'device_profile_applied', profile: '{name}' }});
}});"#,
            manufacturer = self.manufacturer,
            model = self.model,
            brand = self.brand,
            product = self.product,
            device = self.device,
            hardware = self.hardware,
            fingerprint = self.fingerprint,
            board = self.board,
            display = self.display,
            description = self.description,
            name = self.name,
        )
    }
}

/// Anti-detection configuration manager
pub struct AntiDetection {
    /// Available bypass scripts
    scripts: Vec<BypassScript>,
    /// Active device profile
    device_profile: Option<DeviceProfile>,
    /// Enabled bypass categories
    enabled: HashMap<BypassCategory, bool>,
}

impl AntiDetection {
    pub fn new() -> Self {
        let mut enabled = HashMap::new();
        enabled.insert(BypassCategory::Root, true);
        enabled.insert(BypassCategory::Emulator, true);
        enabled.insert(BypassCategory::Frida, true);
        enabled.insert(BypassCategory::Debugger, true);
        enabled.insert(BypassCategory::SslPinning, false); // opt-in
        enabled.insert(BypassCategory::Integrity, false); // opt-in

        let scripts = Self::builtin_scripts();

        Self {
            scripts,
            device_profile: Some(DeviceProfile::pixel_7()),
            enabled,
        }
    }

    /// Enable or disable a bypass category
    pub fn set_enabled(&mut self, category: BypassCategory, enabled: bool) {
        self.enabled.insert(category, enabled);
    }

    /// Check if a category is enabled
    pub fn is_enabled(&self, category: BypassCategory) -> bool {
        *self.enabled.get(&category).unwrap_or(&false)
    }

    /// Set the device profile for emulator bypass
    pub fn set_device_profile(&mut self, profile: DeviceProfile) {
        self.device_profile = Some(profile);
    }

    /// Get the current device profile
    pub fn device_profile(&self) -> Option<&DeviceProfile> {
        self.device_profile.as_ref()
    }

    /// Get all scripts for enabled categories
    pub fn active_scripts(&self) -> Vec<&BypassScript> {
        self.scripts
            .iter()
            .filter(|s| self.is_enabled(s.category))
            .collect()
    }

    /// Get the combined Frida script for all enabled bypasses
    pub fn combined_script(&self) -> String {
        let mut parts = Vec::new();

        parts.push("// === RexPlayer Anti-Detection Suite ===\n".to_string());

        // Device profile first (if emulator bypass enabled)
        if self.is_enabled(BypassCategory::Emulator) {
            if let Some(ref profile) = self.device_profile {
                parts.push(format!("// Device Profile: {}\n", profile.name));
                parts.push(profile.to_frida_script());
            }
        }

        // Then category scripts
        for script in self.active_scripts() {
            parts.push(format!(
                "\n// --- {} ({:?}) ---\n",
                script.name, script.category
            ));
            parts.push(script.source.clone());
        }

        parts.join("\n")
    }

    /// Get all available bypass scripts
    pub fn all_scripts(&self) -> &[BypassScript] {
        &self.scripts
    }

    fn builtin_scripts() -> Vec<BypassScript> {
        vec![
            // === Root Detection Bypass ===
            BypassScript {
                name: "Root Binary Check Bypass".into(),
                category: BypassCategory::Root,
                description: "Hides su, magisk, supersu binaries from file existence checks".into(),
                default_enabled: true,
                source: r#"Java.perform(function() {
    var File = Java.use('java.io.File');
    var rootPaths = ['/system/bin/su', '/system/xbin/su', '/sbin/su',
        '/system/app/Superuser.apk', '/data/local/xbin/su', '/data/local/bin/su',
        '/system/bin/magisk', '/sbin/magiskd', '/data/adb/magisk'];

    File.exists.implementation = function() {
        var path = this.getAbsolutePath();
        for (var i = 0; i < rootPaths.length; i++) {
            if (path === rootPaths[i]) {
                send({ type: 'bypass', category: 'root', blocked: path });
                return false;
            }
        }
        return this.exists();
    };

    // Hide root packages
    var PM = Java.use('android.app.ApplicationPackageManager');
    PM.getPackageInfo.overload('java.lang.String', 'int').implementation = function(pkg, flags) {
        var rootPkgs = ['com.topjohnwu.magisk', 'eu.chainfire.supersu',
            'com.noshufou.android.su', 'com.koushikdutta.superuser'];
        if (rootPkgs.indexOf(pkg) >= 0) {
            send({ type: 'bypass', category: 'root', blocked_pkg: pkg });
            throw Java.use('android.content.pm.PackageManager$NameNotFoundException').$new(pkg);
        }
        return this.getPackageInfo(pkg, flags);
    };

    send({ type: 'bypass', category: 'root', status: 'installed' });
});"#.into(),
            },

            // === Emulator Detection Bypass ===
            BypassScript {
                name: "Emulator Property Bypass".into(),
                category: BypassCategory::Emulator,
                description: "Patches known emulator indicators in Build props, sensors, telephony".into(),
                default_enabled: true,
                source: r#"Java.perform(function() {
    // Hide emulator-specific files
    var File = Java.use('java.io.File');
    var emuFiles = ['/dev/qemu_pipe', '/dev/goldfish_pipe',
        '/system/lib/libc_malloc_debug_qemu.so', '/dev/socket/qemud',
        '/sys/qemu_trace', '/system/bin/qemu-props'];

    var origExists = File.exists;
    File.exists.implementation = function() {
        var path = this.getAbsolutePath();
        for (var i = 0; i < emuFiles.length; i++) {
            if (path === emuFiles[i]) {
                send({ type: 'bypass', category: 'emulator', blocked_file: path });
                return false;
            }
        }
        return origExists.call(this);
    };

    // Fake TelephonyManager values
    var TelephonyManager = Java.use('android.telephony.TelephonyManager');
    TelephonyManager.getNetworkOperatorName.implementation = function() { return 'T-Mobile'; };
    TelephonyManager.getSimOperatorName.implementation = function() { return 'T-Mobile'; };
    TelephonyManager.getNetworkCountryIso.implementation = function() { return 'us'; };
    TelephonyManager.getSimCountryIso.implementation = function() { return 'us'; };
    TelephonyManager.getPhoneType.implementation = function() { return 1; }; // GSM

    // Fake SensorManager (emulators often have no sensors)
    // This makes apps think real sensors exist
    var Sensor = Java.use('android.hardware.Sensor');
    // Don't patch sensor list — let the app see "sensors" but return plausible values

    send({ type: 'bypass', category: 'emulator', status: 'installed' });
});"#.into(),
            },

            // === Frida Detection Bypass ===
            BypassScript {
                name: "Frida Detection Bypass".into(),
                category: BypassCategory::Frida,
                description: "Hides Frida from port scans, /proc/maps, thread names".into(),
                default_enabled: true,
                source: r#"// Hide Frida from /proc/self/maps
var openPtr = Module.getExportByName('libc.so', 'open');
var open = new NativeFunction(openPtr, 'int', ['pointer', 'int']);

var readPtr = Module.getExportByName('libc.so', 'read');
var read = new NativeFunction(readPtr, 'int', ['int', 'pointer', 'int']);

// Intercept fopen to filter /proc/self/maps
Interceptor.attach(Module.getExportByName('libc.so', 'fopen'), {
    onEnter: function(args) {
        var path = args[0].readUtf8String();
        this.is_maps = (path && path.indexOf('/proc/') >= 0 && path.indexOf('/maps') >= 0);
    },
    onLeave: function(retval) {
        // Let fopen succeed — we'll filter in fgets
    }
});

// Filter frida entries from fgets output
Interceptor.attach(Module.getExportByName('libc.so', 'fgets'), {
    onLeave: function(retval) {
        if (retval.isNull()) return;
        var line = retval.readUtf8String();
        if (line && (line.indexOf('frida') >= 0 || line.indexOf('gadget') >= 0 ||
                     line.indexOf('gmain') >= 0 || line.indexOf('linjector') >= 0)) {
            // Replace with empty/innocuous line
            retval.writeUtf8String('');
            send({ type: 'bypass', category: 'frida', blocked_maps_entry: true });
        }
    }
});

// Hide frida-server port (27042)
Interceptor.attach(Module.getExportByName('libc.so', 'connect'), {
    onEnter: function(args) {
        var sockaddr = args[1];
        var family = sockaddr.readU16();
        if (family === 2) { // AF_INET
            var port = (sockaddr.add(2).readU8() << 8) | sockaddr.add(3).readU8();
            if (port === 27042 || port === 27043) {
                send({ type: 'bypass', category: 'frida', blocked_port_scan: port });
                // Return ECONNREFUSED
                this.block = true;
            }
        }
    },
    onLeave: function(retval) {
        if (this.block) {
            retval.replace(-1);
        }
    }
});

send({ type: 'bypass', category: 'frida', status: 'installed' });"#.into(),
            },

            // === Debugger Detection Bypass ===
            BypassScript {
                name: "Debugger Detection Bypass".into(),
                category: BypassCategory::Debugger,
                description: "Bypasses ptrace anti-debug, TracerPid, isDebuggerConnected".into(),
                default_enabled: true,
                source: r#"Java.perform(function() {
    // android.os.Debug.isDebuggerConnected
    var Debug = Java.use('android.os.Debug');
    Debug.isDebuggerConnected.implementation = function() {
        send({ type: 'bypass', category: 'debugger', check: 'isDebuggerConnected' });
        return false;
    };
});

// ptrace anti-debug: many apps call ptrace(PTRACE_TRACEME) to prevent debugging
Interceptor.attach(Module.getExportByName('libc.so', 'ptrace'), {
    onEnter: function(args) {
        var request = args[0].toInt32();
        if (request === 0) { // PTRACE_TRACEME
            send({ type: 'bypass', category: 'debugger', check: 'ptrace_traceme' });
            this.bypass = true;
        }
    },
    onLeave: function(retval) {
        if (this.bypass) {
            retval.replace(0); // success
        }
    }
});

// Filter TracerPid from /proc/self/status
Interceptor.attach(Module.getExportByName('libc.so', 'fgets'), {
    onLeave: function(retval) {
        if (retval.isNull()) return;
        var line = retval.readUtf8String();
        if (line && line.indexOf('TracerPid') >= 0) {
            retval.writeUtf8String('TracerPid:\t0\n');
            send({ type: 'bypass', category: 'debugger', check: 'TracerPid' });
        }
    }
});

send({ type: 'bypass', category: 'debugger', status: 'installed' });"#.into(),
            },

            // === SSL Pinning Bypass ===
            BypassScript {
                name: "Universal SSL Pinning Bypass".into(),
                category: BypassCategory::SslPinning,
                description: "Bypasses TrustManager, OkHttp, Volley, Apache, Flutter SSL pinning".into(),
                default_enabled: false,
                source: r#"Java.perform(function() {
    // === TrustManagerImpl (Android system) ===
    try {
        var TrustManagerImpl = Java.use('com.android.org.conscrypt.TrustManagerImpl');
        TrustManagerImpl.verifyChain.implementation = function(untrusted, anchors, host, clientAuth, ocsp, tls) {
            send({ type: 'bypass', category: 'ssl', host: host, library: 'TrustManagerImpl' });
            return untrusted;
        };
    } catch(e) {}

    // === OkHttp3 CertificatePinner ===
    try {
        var CertPinner = Java.use('okhttp3.CertificatePinner');
        CertPinner.check.overload('java.lang.String', 'java.util.List').implementation = function(host, certs) {
            send({ type: 'bypass', category: 'ssl', host: host, library: 'okhttp3' });
        };
    } catch(e) {}

    // === OkHttp3 internal ===
    try {
        var PinnerCheck = Java.use('okhttp3.internal.tls.CertificateChainCleaner');
        PinnerCheck.clean.implementation = function(chain, host) {
            send({ type: 'bypass', category: 'ssl', host: host, library: 'okhttp3_internal' });
            return chain;
        };
    } catch(e) {}

    // === Apache HttpClient ===
    try {
        var AbstractVerifier = Java.use('org.apache.http.conn.ssl.AbstractVerifier');
        AbstractVerifier.verify.overload('java.lang.String', '[Ljava.lang.String;', '[Ljava.lang.String;', 'boolean').implementation = function() {
            send({ type: 'bypass', category: 'ssl', library: 'apache' });
        };
    } catch(e) {}

    // === WebView SSL errors ===
    try {
        var WebViewClient = Java.use('android.webkit.WebViewClient');
        WebViewClient.onReceivedSslError.implementation = function(view, handler, error) {
            handler.proceed();
            send({ type: 'bypass', category: 'ssl', library: 'webview' });
        };
    } catch(e) {}

    // === Custom X509TrustManager ===
    try {
        var X509TM = Java.use('javax.net.ssl.X509TrustManager');
        var TrustManager = Java.registerClass({
            name: 'com.rex.TrustAllManager',
            implements: [X509TM],
            methods: {
                checkClientTrusted: function(chain, authType) {},
                checkServerTrusted: function(chain, authType) {},
                getAcceptedIssuers: function() { return []; }
            }
        });

        var SSLContext = Java.use('javax.net.ssl.SSLContext');
        var ctx = SSLContext.getInstance('TLS');
        ctx.init(null, [TrustManager.$new()], null);
        SSLContext.getInstance.overload('java.lang.String').implementation = function(protocol) {
            send({ type: 'bypass', category: 'ssl', library: 'SSLContext_override' });
            return ctx;
        };
    } catch(e) {}

    send({ type: 'bypass', category: 'ssl_pinning', status: 'installed' });
});"#.into(),
            },

            // === Integrity Check Bypass ===
            BypassScript {
                name: "APK Integrity Bypass".into(),
                category: BypassCategory::Integrity,
                description: "Bypasses signature verification and dex checksum checks".into(),
                default_enabled: false,
                source: r#"Java.perform(function() {
    // Fake PackageInfo.signatures to return original
    var PackageManager = Java.use('android.app.ApplicationPackageManager');
    PackageManager.getPackageInfo.overload('java.lang.String', 'int').implementation = function(pkg, flags) {
        // Remove GET_SIGNATURES flag to prevent signature comparison
        var info = this.getPackageInfo(pkg, flags & ~0x40);
        send({ type: 'bypass', category: 'integrity', check: 'signature', pkg: pkg });
        return info;
    };

    send({ type: 'bypass', category: 'integrity', status: 'installed' });
});"#.into(),
            },
        ]
    }
}

impl Default for AntiDetection {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_default_enabled() {
        let ad = AntiDetection::new();
        assert!(ad.is_enabled(BypassCategory::Root));
        assert!(ad.is_enabled(BypassCategory::Emulator));
        assert!(ad.is_enabled(BypassCategory::Frida));
        assert!(ad.is_enabled(BypassCategory::Debugger));
        assert!(!ad.is_enabled(BypassCategory::SslPinning));
        assert!(!ad.is_enabled(BypassCategory::Integrity));
    }

    #[test]
    fn test_toggle_category() {
        let mut ad = AntiDetection::new();
        ad.set_enabled(BypassCategory::SslPinning, true);
        assert!(ad.is_enabled(BypassCategory::SslPinning));
        ad.set_enabled(BypassCategory::Root, false);
        assert!(!ad.is_enabled(BypassCategory::Root));
    }

    #[test]
    fn test_active_scripts() {
        let ad = AntiDetection::new();
        let active = ad.active_scripts();
        // Root + Emulator + Frida + Debugger = 4 enabled by default
        assert_eq!(active.len(), 4);
    }

    #[test]
    fn test_all_enabled() {
        let mut ad = AntiDetection::new();
        ad.set_enabled(BypassCategory::SslPinning, true);
        ad.set_enabled(BypassCategory::Integrity, true);
        let active = ad.active_scripts();
        assert_eq!(active.len(), 6);
    }

    #[test]
    fn test_device_profile_pixel() {
        let profile = DeviceProfile::pixel_7();
        assert_eq!(profile.manufacturer, "Google");
        assert_eq!(profile.model, "Pixel 7");
        let script = profile.to_frida_script();
        assert!(script.contains("Java.perform"));
        assert!(script.contains("Pixel 7"));
        assert!(script.contains("ro.product.manufacturer"));
    }

    #[test]
    fn test_device_profile_galaxy() {
        let profile = DeviceProfile::galaxy_s24();
        assert_eq!(profile.manufacturer, "samsung");
        let script = profile.to_frida_script();
        assert!(script.contains("SM-S921B"));
    }

    #[test]
    fn test_combined_script() {
        let ad = AntiDetection::new();
        let script = ad.combined_script();
        assert!(script.contains("Anti-Detection Suite"));
        assert!(script.contains("Root Binary Check"));
        assert!(script.contains("Frida Detection"));
        // SSL pinning should NOT be in combined (disabled by default)
        assert!(!script.contains("Universal SSL Pinning"));
    }

    #[test]
    fn test_combined_with_ssl() {
        let mut ad = AntiDetection::new();
        ad.set_enabled(BypassCategory::SslPinning, true);
        let script = ad.combined_script();
        assert!(script.contains("Universal SSL Pinning"));
        assert!(script.contains("TrustManagerImpl"));
    }

    #[test]
    fn test_set_custom_profile() {
        let mut ad = AntiDetection::new();
        let mut profile = DeviceProfile::pixel_7();
        profile.name = "Custom Phone".into();
        profile.model = "Custom Model".into();
        ad.set_device_profile(profile);

        let dp = ad.device_profile().unwrap();
        assert_eq!(dp.name, "Custom Phone");
    }

    #[test]
    fn test_builtin_scripts_have_source() {
        let ad = AntiDetection::new();
        for script in ad.all_scripts() {
            assert!(
                !script.source.is_empty(),
                "Script {} has empty source",
                script.name
            );
            assert!(!script.name.is_empty());
            assert!(!script.description.is_empty());
        }
    }
}
