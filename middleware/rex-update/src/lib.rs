//! RexPlayer self-update mechanism

pub struct UpdateInfo {
    pub current_version: String,
    pub latest_version: String,
    pub download_url: String,
    pub changelog: String,
}
