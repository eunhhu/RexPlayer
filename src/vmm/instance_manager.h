#pragma once

#include "rex/hal/types.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::vmm {

/// Instance lifecycle states
enum class InstanceState : uint32_t {
    Created,
    Starting,
    Running,
    Paused,
    Stopping,
    Stopped,
    Error,
};

/// Convert InstanceState to string
inline const char* instance_state_str(InstanceState s) {
    switch (s) {
        case InstanceState::Created:  return "Created";
        case InstanceState::Starting: return "Starting";
        case InstanceState::Running:  return "Running";
        case InstanceState::Paused:   return "Paused";
        case InstanceState::Stopping: return "Stopping";
        case InstanceState::Stopped:  return "Stopped";
        case InstanceState::Error:    return "Error";
    }
    return "Unknown";
}

/// Metadata and configuration for a single VM instance
struct InstanceInfo {
    /// Unique instance identifier (UUID-like string)
    std::string id;
    /// Human-readable instance name
    std::string name;
    /// Current lifecycle state
    InstanceState state = InstanceState::Created;
    /// Path to the TOML configuration file
    std::string config_path;
    /// RAM allocation in megabytes
    uint64_t ram_mb = 2048;
    /// Number of virtual CPUs
    uint32_t vcpu_count = 2;
    /// Path to the system disk image
    std::string system_image;
    /// Path to the data disk image (optional)
    std::string data_image;
    /// ISO 8601 creation timestamp
    std::string created_at;
    /// ISO 8601 last-started timestamp (empty if never started)
    std::string last_started;
    /// Error message (populated when state == Error)
    std::string error_message;
};

/// Configuration for creating a new instance
struct InstanceCreateConfig {
    /// Human-readable name (must be unique)
    std::string name;
    /// Path to the TOML configuration file
    std::string config_path;
    /// RAM in megabytes (0 = use config default)
    uint64_t ram_mb = 0;
    /// Number of vCPUs (0 = use config default)
    uint32_t vcpu_count = 0;
    /// System image path (empty = use config default)
    std::string system_image;
    /// Data image path (empty = none)
    std::string data_image;
};

/// Options for cloning an instance
struct CloneOptions {
    /// Name for the cloned instance
    std::string new_name;
    /// Prefer copy-on-write cloning and fall back to a full copy when unavailable
    bool copy_on_write = true;
    /// Also clone the data image (if any)
    bool clone_data_image = true;
};

/// Maximum number of concurrent instances
static constexpr uint32_t MAX_INSTANCES = 16;

/// Manages multiple VM instance lifecycles
///
/// Tracks creation, startup, shutdown, and destruction of VM instances.
/// Supports instance persistence (save/load instance list to JSON),
/// resource isolation, and best-effort copy-on-write cloning.
class InstanceManager {
public:
    InstanceManager();
    ~InstanceManager();

    // Non-copyable
    InstanceManager(const InstanceManager&) = delete;
    InstanceManager& operator=(const InstanceManager&) = delete;

    /// Create a new VM instance
    /// @return instance ID on success, HalError on failure
    rex::hal::HalResult<std::string> create_instance(const InstanceCreateConfig& config);

    /// Destroy an instance (must be stopped first)
    rex::hal::HalResult<void> destroy_instance(const std::string& id);

    /// Start an instance
    rex::hal::HalResult<void> start_instance(const std::string& id);

    /// Stop an instance
    rex::hal::HalResult<void> stop_instance(const std::string& id);

    /// Pause a running instance
    rex::hal::HalResult<void> pause_instance(const std::string& id);

    /// Resume a paused instance
    rex::hal::HalResult<void> resume_instance(const std::string& id);

    /// List all instances
    std::vector<InstanceInfo> list_instances() const;

    /// Get info for a specific instance
    rex::hal::HalResult<InstanceInfo> get_instance(const std::string& id) const;

    /// Clone an existing instance (copy-on-write disk images)
    /// @return new instance ID on success
    rex::hal::HalResult<std::string> clone_instance(const std::string& source_id,
                                                      const CloneOptions& options);

    /// Get the number of active (non-stopped, non-error) instances
    uint32_t active_count() const;

    /// Get the total number of instances
    uint32_t total_count() const;

    /// Save the instance list to a JSON file for persistence
    rex::hal::HalResult<void> save_state(const std::string& path) const;

    /// Load the instance list from a previously saved JSON file
    rex::hal::HalResult<void> load_state(const std::string& path);

    /// Set the base directory for instance data (disk images, configs)
    void set_data_dir(const std::string& path);
    const std::string& data_dir() const { return data_dir_; }

private:
    /// Generate a new UUID-like instance ID
    std::string generate_id();

    /// Get current ISO 8601 timestamp
    std::string current_timestamp();

    /// Copy a file, optionally using copy-on-write
    bool copy_file(const std::string& src, const std::string& dst, bool cow);

    /// Find instance by ID (caller must hold mutex_)
    InstanceInfo* find_instance(const std::string& id);
    const InstanceInfo* find_instance(const std::string& id) const;

    /// Find instance by name (caller must hold mutex_)
    const InstanceInfo* find_by_name(const std::string& name) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceInfo> instances_;
    std::string data_dir_;
    uint32_t id_counter_ = 0;
};

} // namespace rex::vmm
