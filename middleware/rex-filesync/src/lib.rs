//! Host ↔ Guest file synchronization
//!
//! Provides shared folders and drag-and-drop file transfer between
//! the host and Android guest via virtio-vsock or 9p filesystem.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use thiserror::Error;

#[derive(Error, Debug)]
pub enum FileSyncError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("Shared folder not found: {0}")]
    NotFound(String),

    #[error("Path outside shared folder")]
    PathTraversal,

    #[error("Folder already shared: {0}")]
    AlreadyShared(String),

    #[error("Transfer failed: {0}")]
    TransferFailed(String),
}

pub type FileSyncResult<T> = Result<T, FileSyncError>;

/// Shared folder configuration
#[derive(Debug, Clone)]
pub struct SharedFolder {
    /// Unique name for this share
    pub name: String,
    /// Path on the host filesystem
    pub host_path: PathBuf,
    /// Mount path inside the guest
    pub guest_path: String,
    /// Whether the guest can write to this share
    pub writable: bool,
}

/// File transfer direction
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransferDirection {
    HostToGuest,
    GuestToHost,
}

/// Status of a file transfer
#[derive(Debug, Clone)]
pub struct TransferStatus {
    pub id: u64,
    pub filename: String,
    pub direction: TransferDirection,
    pub total_bytes: u64,
    pub transferred_bytes: u64,
    pub completed: bool,
    pub error: Option<String>,
}

impl TransferStatus {
    pub fn progress_percent(&self) -> f64 {
        if self.total_bytes == 0 {
            return 0.0;
        }
        (self.transferred_bytes as f64 / self.total_bytes as f64) * 100.0
    }
}

/// File synchronization manager
pub struct FileSyncManager {
    /// Registered shared folders (name → SharedFolder)
    shares: HashMap<String, SharedFolder>,
    /// Active transfers
    transfers: HashMap<u64, TransferStatus>,
    /// Next transfer ID
    next_transfer_id: u64,
}

impl FileSyncManager {
    pub fn new() -> Self {
        Self {
            shares: HashMap::new(),
            transfers: HashMap::new(),
            next_transfer_id: 1,
        }
    }

    /// Add a shared folder
    pub fn add_share(&mut self, folder: SharedFolder) -> FileSyncResult<()> {
        if self.shares.contains_key(&folder.name) {
            return Err(FileSyncError::AlreadyShared(folder.name.clone()));
        }
        self.shares.insert(folder.name.clone(), folder);
        Ok(())
    }

    /// Remove a shared folder
    pub fn remove_share(&mut self, name: &str) -> FileSyncResult<()> {
        self.shares
            .remove(name)
            .map(|_| ())
            .ok_or_else(|| FileSyncError::NotFound(name.to_string()))
    }

    /// Get a shared folder by name
    pub fn get_share(&self, name: &str) -> Option<&SharedFolder> {
        self.shares.get(name)
    }

    /// List all shared folders
    pub fn list_shares(&self) -> Vec<&SharedFolder> {
        self.shares.values().collect()
    }

    /// Validate a guest path within a share (prevent directory traversal)
    pub fn validate_path(&self, share_name: &str, relative_path: &str) -> FileSyncResult<PathBuf> {
        let share = self
            .shares
            .get(share_name)
            .ok_or_else(|| FileSyncError::NotFound(share_name.to_string()))?;

        let full_path = share.host_path.join(relative_path);
        let canonical = full_path
            .canonicalize()
            .unwrap_or_else(|_| full_path.clone());

        // Ensure resolved path is within the share
        if !canonical.starts_with(&share.host_path) {
            return Err(FileSyncError::PathTraversal);
        }

        Ok(canonical)
    }

    /// Start a file transfer (returns transfer ID)
    pub fn start_transfer(
        &mut self,
        filename: String,
        direction: TransferDirection,
        total_bytes: u64,
    ) -> u64 {
        let id = self.next_transfer_id;
        self.next_transfer_id += 1;

        self.transfers.insert(
            id,
            TransferStatus {
                id,
                filename,
                direction,
                total_bytes,
                transferred_bytes: 0,
                completed: false,
                error: None,
            },
        );

        id
    }

    /// Update transfer progress
    pub fn update_transfer(&mut self, id: u64, bytes: u64) -> Option<&TransferStatus> {
        if let Some(transfer) = self.transfers.get_mut(&id) {
            transfer.transferred_bytes = bytes;
            if bytes >= transfer.total_bytes {
                transfer.completed = true;
            }
            Some(transfer)
        } else {
            None
        }
    }

    /// Mark a transfer as failed
    pub fn fail_transfer(&mut self, id: u64, error: String) {
        if let Some(transfer) = self.transfers.get_mut(&id) {
            transfer.error = Some(error);
            transfer.completed = true;
        }
    }

    /// Get transfer status
    pub fn get_transfer(&self, id: u64) -> Option<&TransferStatus> {
        self.transfers.get(&id)
    }

    /// List active (incomplete) transfers
    pub fn active_transfers(&self) -> Vec<&TransferStatus> {
        self.transfers
            .values()
            .filter(|t| !t.completed)
            .collect()
    }

    /// Clean up completed transfers
    pub fn cleanup_completed(&mut self) -> usize {
        let before = self.transfers.len();
        self.transfers.retain(|_, t| !t.completed);
        before - self.transfers.len()
    }
}

impl Default for FileSyncManager {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_share() -> SharedFolder {
        SharedFolder {
            name: "photos".into(),
            host_path: PathBuf::from("/tmp/rex_test_share"),
            guest_path: "/sdcard/Pictures".into(),
            writable: true,
        }
    }

    #[test]
    fn test_add_remove_share() {
        let mut mgr = FileSyncManager::new();
        mgr.add_share(test_share()).unwrap();
        assert_eq!(mgr.list_shares().len(), 1);

        mgr.remove_share("photos").unwrap();
        assert_eq!(mgr.list_shares().len(), 0);
    }

    #[test]
    fn test_duplicate_share() {
        let mut mgr = FileSyncManager::new();
        mgr.add_share(test_share()).unwrap();
        assert!(mgr.add_share(test_share()).is_err());
    }

    #[test]
    fn test_remove_nonexistent() {
        let mut mgr = FileSyncManager::new();
        assert!(mgr.remove_share("nope").is_err());
    }

    #[test]
    fn test_transfer_lifecycle() {
        let mut mgr = FileSyncManager::new();

        let id = mgr.start_transfer("photo.jpg".into(), TransferDirection::HostToGuest, 1000);
        assert_eq!(mgr.active_transfers().len(), 1);

        let status = mgr.update_transfer(id, 500).unwrap();
        assert_eq!(status.progress_percent(), 50.0);
        assert!(!status.completed);

        mgr.update_transfer(id, 1000);
        let status = mgr.get_transfer(id).unwrap();
        assert!(status.completed);
        assert_eq!(status.progress_percent(), 100.0);
    }

    #[test]
    fn test_transfer_failure() {
        let mut mgr = FileSyncManager::new();
        let id = mgr.start_transfer("file.apk".into(), TransferDirection::GuestToHost, 5000);

        mgr.fail_transfer(id, "connection lost".into());
        let status = mgr.get_transfer(id).unwrap();
        assert!(status.completed);
        assert_eq!(status.error.as_deref(), Some("connection lost"));
    }

    #[test]
    fn test_cleanup_completed() {
        let mut mgr = FileSyncManager::new();
        let id1 = mgr.start_transfer("a.txt".into(), TransferDirection::HostToGuest, 100);
        let _id2 = mgr.start_transfer("b.txt".into(), TransferDirection::HostToGuest, 200);

        mgr.update_transfer(id1, 100); // complete
        let removed = mgr.cleanup_completed();
        assert_eq!(removed, 1);
        assert_eq!(mgr.active_transfers().len(), 1);
    }

    #[test]
    fn test_get_share() {
        let mut mgr = FileSyncManager::new();
        mgr.add_share(test_share()).unwrap();
        let share = mgr.get_share("photos").unwrap();
        assert_eq!(share.guest_path, "/sdcard/Pictures");
        assert!(share.writable);
    }
}
