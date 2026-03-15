//! Frida Server management and auto-update
//!
//! Manages Frida Server lifecycle inside the guest, including:
//! - Version checking via GitHub API
//! - Automatic download and installation
//! - vsock bridge (guest:27042 ↔ host:27042)

pub mod manager;

pub use manager::FridaManager;
