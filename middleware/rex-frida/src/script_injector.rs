//! Frida Script Injection
//!
//! Manages loading, injecting, and controlling Frida scripts in the Android guest.
//! Scripts are sent to the Frida Server running inside the guest via vsock.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use thiserror::Error;

#[derive(Error, Debug)]
pub enum ScriptError {
    #[error("Script not found: {0}")]
    NotFound(String),

    #[error("Failed to load script: {0}")]
    LoadFailed(String),

    #[error("Injection failed: {0}")]
    InjectionFailed(String),

    #[error("Script compilation error: {0}")]
    CompileError(String),

    #[error("Target process not found: {0}")]
    ProcessNotFound(String),

    #[error("Not connected to Frida server")]
    NotConnected,

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
}

pub type ScriptResult<T> = Result<T, ScriptError>;

/// How to identify the target process
#[derive(Debug, Clone)]
pub enum TargetProcess {
    /// Attach by process ID
    Pid(u32),
    /// Attach by process name
    Name(String),
    /// Attach by package name (Android)
    Package(String),
    /// Spawn a new process
    Spawn(String),
}

impl std::fmt::Display for TargetProcess {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TargetProcess::Pid(pid) => write!(f, "PID:{}", pid),
            TargetProcess::Name(n) => write!(f, "name:{}", n),
            TargetProcess::Package(p) => write!(f, "pkg:{}", p),
            TargetProcess::Spawn(s) => write!(f, "spawn:{}", s),
        }
    }
}

/// Script execution mode
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScriptRuntime {
    /// Default V8-based runtime
    Default,
    /// QuickJS runtime (faster startup, lower memory)
    QJS,
}

/// State of an injected script
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScriptState {
    /// Loaded but not yet injected
    Loaded,
    /// Injected and running
    Running,
    /// Paused (breakpoint or explicit pause)
    Paused,
    /// Stopped (detached or error)
    Stopped,
    /// Compilation/load error
    Error,
}

/// A single Frida script instance
#[derive(Debug, Clone)]
pub struct ScriptInstance {
    /// Unique script ID
    pub id: u64,
    /// Script name (filename or user-provided)
    pub name: String,
    /// Script source code
    pub source: String,
    /// Target process
    pub target: TargetProcess,
    /// Runtime engine
    pub runtime: ScriptRuntime,
    /// Current state
    pub state: ScriptState,
    /// Collected messages from the script (JSON strings)
    pub messages: Vec<String>,
    /// Error message (if state == Error)
    pub error: Option<String>,
}

/// Configuration for script injection
#[derive(Debug, Clone)]
pub struct InjectConfig {
    /// Target process
    pub target: TargetProcess,
    /// Script runtime
    pub runtime: ScriptRuntime,
    /// Whether to pause the process on attach
    pub pause_on_attach: bool,
    /// Whether to automatically resume after injection
    pub auto_resume: bool,
    /// Script name (for identification)
    pub name: Option<String>,
}

impl Default for InjectConfig {
    fn default() -> Self {
        Self {
            target: TargetProcess::Name("system_server".into()),
            runtime: ScriptRuntime::Default,
            pause_on_attach: false,
            auto_resume: true,
            name: None,
        }
    }
}

/// Manages Frida script injection into the Android guest
pub struct ScriptInjector {
    /// Active script instances
    scripts: HashMap<u64, ScriptInstance>,
    /// Next script ID
    next_id: u64,
    /// Connected to Frida server
    connected: bool,
    /// Script search paths
    search_paths: Vec<PathBuf>,
    /// Message callback (script_id, message_json)
    on_message: Option<Box<dyn Fn(u64, &str) + Send>>,
}

impl ScriptInjector {
    pub fn new() -> Self {
        Self {
            scripts: HashMap::new(),
            next_id: 1,
            connected: false,
            search_paths: Vec::new(),
            on_message: None,
        }
    }

    /// Set the connection state
    pub fn set_connected(&mut self, connected: bool) {
        self.connected = connected;
        if !connected {
            // Mark all running scripts as stopped
            for script in self.scripts.values_mut() {
                if script.state == ScriptState::Running {
                    script.state = ScriptState::Stopped;
                }
            }
        }
    }

    /// Check if connected to Frida server
    pub fn is_connected(&self) -> bool {
        self.connected
    }

    /// Add a script search path
    pub fn add_search_path(&mut self, path: PathBuf) {
        self.search_paths.push(path);
    }

    /// Set the message callback
    pub fn set_on_message<F>(&mut self, callback: F)
    where
        F: Fn(u64, &str) + Send + 'static,
    {
        self.on_message = Some(Box::new(callback));
    }

    /// Load a script from a file
    pub fn load_file(&mut self, path: &Path) -> ScriptResult<u64> {
        let source = std::fs::read_to_string(path)
            .map_err(|e| ScriptError::LoadFailed(e.to_string()))?;

        let name = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("unnamed")
            .to_string();

        Ok(self.load_source(name, source))
    }

    /// Load a script from source code
    pub fn load_source(&mut self, name: String, source: String) -> u64 {
        let id = self.next_id;
        self.next_id += 1;

        let instance = ScriptInstance {
            id,
            name,
            source,
            target: TargetProcess::Name("system_server".into()),
            runtime: ScriptRuntime::Default,
            state: ScriptState::Loaded,
            messages: Vec::new(),
            error: None,
        };

        self.scripts.insert(id, instance);
        id
    }

    /// Find a script file in search paths
    pub fn find_script(&self, filename: &str) -> Option<PathBuf> {
        // Check absolute/relative path first
        let path = Path::new(filename);
        if path.exists() {
            return Some(path.to_path_buf());
        }

        // Search in search paths
        for dir in &self.search_paths {
            let full = dir.join(filename);
            if full.exists() {
                return Some(full);
            }
            // Try with .js extension
            let with_ext = dir.join(format!("{}.js", filename));
            if with_ext.exists() {
                return Some(with_ext);
            }
        }

        None
    }

    /// Inject a loaded script into a target process
    pub fn inject(&mut self, script_id: u64, config: InjectConfig) -> ScriptResult<()> {
        if !self.connected {
            return Err(ScriptError::NotConnected);
        }

        let script = self.scripts.get_mut(&script_id)
            .ok_or_else(|| ScriptError::NotFound(format!("script {}", script_id)))?;

        if script.state != ScriptState::Loaded && script.state != ScriptState::Stopped {
            return Err(ScriptError::InjectionFailed(
                format!("script {} is in state {:?}, expected Loaded or Stopped", script_id, script.state)
            ));
        }

        script.target = config.target;
        script.runtime = config.runtime;
        if let Some(name) = config.name {
            script.name = name;
        }

        // In a real implementation, this would:
        // 1. Connect to Frida Server via vsock:27042
        // 2. frida.attach(target) or frida.spawn(target)
        // 3. session.create_script(source, runtime)
        // 4. script.on('message', callback)
        // 5. script.load()
        // 6. If auto_resume: device.resume(pid)

        script.state = ScriptState::Running;
        script.error = None;

        Ok(())
    }

    /// Quick inject: load source and inject in one call
    pub fn quick_inject(
        &mut self,
        name: &str,
        source: &str,
        target: TargetProcess,
    ) -> ScriptResult<u64> {
        let id = self.load_source(name.to_string(), source.to_string());
        let config = InjectConfig {
            target,
            ..Default::default()
        };
        self.inject(id, config)?;
        Ok(id)
    }

    /// Detach a script from its target process
    pub fn detach(&mut self, script_id: u64) -> ScriptResult<()> {
        let script = self.scripts.get_mut(&script_id)
            .ok_or_else(|| ScriptError::NotFound(format!("script {}", script_id)))?;

        script.state = ScriptState::Stopped;
        Ok(())
    }

    /// Remove a script entirely
    pub fn remove(&mut self, script_id: u64) -> ScriptResult<()> {
        if let Some(script) = self.scripts.get(&script_id) {
            if script.state == ScriptState::Running {
                self.detach(script_id)?;
            }
        }
        self.scripts.remove(&script_id)
            .ok_or_else(|| ScriptError::NotFound(format!("script {}", script_id)))?;
        Ok(())
    }

    /// Deliver a message from a running script
    pub fn deliver_message(&mut self, script_id: u64, message: String) {
        if let Some(script) = self.scripts.get_mut(&script_id) {
            script.messages.push(message.clone());
            if let Some(ref cb) = self.on_message {
                cb(script_id, &message);
            }
        }
    }

    /// Get a script by ID
    pub fn get_script(&self, script_id: u64) -> Option<&ScriptInstance> {
        self.scripts.get(&script_id)
    }

    /// List all scripts
    pub fn list_scripts(&self) -> Vec<&ScriptInstance> {
        self.scripts.values().collect()
    }

    /// List running scripts
    pub fn running_scripts(&self) -> Vec<&ScriptInstance> {
        self.scripts.values()
            .filter(|s| s.state == ScriptState::Running)
            .collect()
    }

    /// Get the number of active scripts
    pub fn active_count(&self) -> usize {
        self.scripts.values()
            .filter(|s| s.state == ScriptState::Running || s.state == ScriptState::Paused)
            .count()
    }

    /// Common Frida script snippets
    pub fn snippet_enumerate_modules() -> &'static str {
        r#"Process.enumerateModules().forEach(function(m) {
    send({ type: 'module', name: m.name, base: m.base, size: m.size });
});"#
    }

    pub fn snippet_hook_function(module: &str, export_name: &str) -> String {
        format!(
            r#"var addr = Module.findExportByName("{module}", "{export_name}");
if (addr) {{
    Interceptor.attach(addr, {{
        onEnter: function(args) {{
            send({{ type: 'call', function: '{export_name}', args: [args[0], args[1]] }});
        }},
        onLeave: function(retval) {{
            send({{ type: 'return', function: '{export_name}', retval: retval }});
        }}
    }});
    send({{ type: 'hooked', function: '{export_name}', address: addr }});
}} else {{
    send({{ type: 'error', message: 'Export not found: {export_name}' }});
}}"#
        )
    }

    pub fn snippet_trace_java_method(class_name: &str, method_name: &str) -> String {
        format!(
            r#"Java.perform(function() {{
    var cls = Java.use("{class_name}");
    cls.{method_name}.implementation = function() {{
        send({{ type: 'java_call', class: '{class_name}', method: '{method_name}', args: Array.from(arguments) }});
        var ret = this.{method_name}.apply(this, arguments);
        send({{ type: 'java_return', class: '{class_name}', method: '{method_name}', retval: ret }});
        return ret;
    }};
    send({{ type: 'java_hooked', class: '{class_name}', method: '{method_name}' }});
}});"#
        )
    }

    pub fn snippet_ssl_pinning_bypass() -> &'static str {
        r#"Java.perform(function() {
    // TrustManager bypass
    var TrustManagerImpl = Java.use('com.android.org.conscrypt.TrustManagerImpl');
    TrustManagerImpl.verifyChain.implementation = function(untrustedChain, trustAnchorChain, host, clientAuth, ocspData, tlsSctData) {
        send({ type: 'ssl_bypass', host: host });
        return untrustedChain;
    };

    // OkHttp CertificatePinner bypass
    try {
        var CertPinner = Java.use('okhttp3.CertificatePinner');
        CertPinner.check.overload('java.lang.String', 'java.util.List').implementation = function(hostname, peerCerts) {
            send({ type: 'ssl_bypass', library: 'okhttp3', host: hostname });
        };
    } catch(e) {}

    send({ type: 'ssl_pinning_bypass_installed' });
});"#
    }
}

impl Default for ScriptInjector {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_source() {
        let mut injector = ScriptInjector::new();
        let id = injector.load_source("test".into(), "console.log('hi')".into());
        assert_eq!(id, 1);

        let script = injector.get_script(id).unwrap();
        assert_eq!(script.name, "test");
        assert_eq!(script.state, ScriptState::Loaded);
        assert_eq!(script.source, "console.log('hi')");
    }

    #[test]
    fn test_inject_requires_connection() {
        let mut injector = ScriptInjector::new();
        let id = injector.load_source("test".into(), "1+1".into());

        let result = injector.inject(id, InjectConfig::default());
        assert!(result.is_err());
        assert!(matches!(result.unwrap_err(), ScriptError::NotConnected));
    }

    #[test]
    fn test_inject_success() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        let id = injector.load_source("test".into(), "1+1".into());
        let config = InjectConfig {
            target: TargetProcess::Package("com.example".into()),
            ..Default::default()
        };

        injector.inject(id, config).unwrap();
        let script = injector.get_script(id).unwrap();
        assert_eq!(script.state, ScriptState::Running);
    }

    #[test]
    fn test_quick_inject() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        let id = injector.quick_inject(
            "enum_modules",
            ScriptInjector::snippet_enumerate_modules(),
            TargetProcess::Pid(1234),
        ).unwrap();

        assert_eq!(injector.active_count(), 1);
        assert_eq!(injector.running_scripts().len(), 1);

        let script = injector.get_script(id).unwrap();
        assert_eq!(script.state, ScriptState::Running);
    }

    #[test]
    fn test_detach() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        let id = injector.quick_inject("test", "1", TargetProcess::Pid(1)).unwrap();
        injector.detach(id).unwrap();

        let script = injector.get_script(id).unwrap();
        assert_eq!(script.state, ScriptState::Stopped);
        assert_eq!(injector.active_count(), 0);
    }

    #[test]
    fn test_remove() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        let id = injector.quick_inject("test", "1", TargetProcess::Pid(1)).unwrap();
        injector.remove(id).unwrap();
        assert!(injector.get_script(id).is_none());
    }

    #[test]
    fn test_message_delivery() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        let id = injector.quick_inject("test", "1", TargetProcess::Pid(1)).unwrap();
        injector.deliver_message(id, r#"{"type":"log","payload":"hello"}"#.into());

        let script = injector.get_script(id).unwrap();
        assert_eq!(script.messages.len(), 1);
        assert!(script.messages[0].contains("hello"));
    }

    #[test]
    fn test_disconnect_stops_scripts() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        injector.quick_inject("a", "1", TargetProcess::Pid(1)).unwrap();
        injector.quick_inject("b", "2", TargetProcess::Pid(2)).unwrap();
        assert_eq!(injector.active_count(), 2);

        injector.set_connected(false);
        assert_eq!(injector.active_count(), 0);
    }

    #[test]
    fn test_hook_function_snippet() {
        let snippet = ScriptInjector::snippet_hook_function("libc.so", "open");
        assert!(snippet.contains("libc.so"));
        assert!(snippet.contains("open"));
        assert!(snippet.contains("Interceptor.attach"));
    }

    #[test]
    fn test_java_trace_snippet() {
        let snippet = ScriptInjector::snippet_trace_java_method(
            "android.app.Activity", "onCreate");
        assert!(snippet.contains("Java.perform"));
        assert!(snippet.contains("android.app.Activity"));
        assert!(snippet.contains("onCreate"));
    }

    #[test]
    fn test_ssl_pinning_bypass() {
        let snippet = ScriptInjector::snippet_ssl_pinning_bypass();
        assert!(snippet.contains("TrustManagerImpl"));
        assert!(snippet.contains("CertificatePinner"));
    }

    #[test]
    fn test_search_path() {
        let mut injector = ScriptInjector::new();
        injector.add_search_path(PathBuf::from("/nonexistent"));
        assert!(injector.find_script("nope.js").is_none());
    }

    #[test]
    fn test_list_scripts() {
        let mut injector = ScriptInjector::new();
        injector.load_source("a".into(), "1".into());
        injector.load_source("b".into(), "2".into());
        assert_eq!(injector.list_scripts().len(), 2);
    }

    #[test]
    fn test_target_display() {
        assert_eq!(TargetProcess::Pid(42).to_string(), "PID:42");
        assert_eq!(TargetProcess::Name("app".into()).to_string(), "name:app");
        assert_eq!(TargetProcess::Package("com.x".into()).to_string(), "pkg:com.x");
    }

    #[test]
    fn test_cannot_inject_running_script() {
        let mut injector = ScriptInjector::new();
        injector.set_connected(true);

        let id = injector.quick_inject("test", "1", TargetProcess::Pid(1)).unwrap();
        let result = injector.inject(id, InjectConfig::default());
        assert!(result.is_err());
    }
}
