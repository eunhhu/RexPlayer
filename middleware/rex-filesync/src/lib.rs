//! Host ↔ Guest file synchronization (shared folders, drag & drop)

pub struct SharedFolder {
    pub host_path: String,
    pub guest_path: String,
    pub writable: bool,
}
