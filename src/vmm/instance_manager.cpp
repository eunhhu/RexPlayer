#include "instance_manager.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <copyfile.h>
#endif

namespace fs = std::filesystem;

namespace rex::vmm {

// ============================================================================
// Construction
// ============================================================================

InstanceManager::InstanceManager() {
    // Default data directory: ~/.rexplayer/instances
    const char* home = nullptr;
#ifdef _WIN32
    home = std::getenv("USERPROFILE");
#else
    home = std::getenv("HOME");
#endif
    if (home) {
        data_dir_ = std::string(home) + "/.rexplayer/instances";
    } else {
        data_dir_ = "/tmp/rexplayer/instances";
    }
}

InstanceManager::~InstanceManager() = default;

// ============================================================================
// ID generation
// ============================================================================

std::string InstanceManager::generate_id() {
    // Generate a UUID v4-like string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 15);
    std::uniform_int_distribution<uint32_t> dist2(8, 11); // variant bits

    const char hex[] = "0123456789abcdef";
    std::string uuid(36, '-');

    // Positions in UUID format: 8-4-4-4-12
    //                           0       8 9      13 14     18 19     23 24           36
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid[i] = '-';
        } else if (i == 14) {
            uuid[i] = '4'; // Version 4
        } else if (i == 19) {
            uuid[i] = hex[dist2(gen)]; // Variant
        } else {
            uuid[i] = hex[dist(gen)];
        }
    }

    return uuid;
}

// ============================================================================
// Timestamp
// ============================================================================

std::string InstanceManager::current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time_t);
#else
    gmtime_r(&time_t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ============================================================================
// File copy (with optional CoW)
// ============================================================================

bool InstanceManager::copy_file(const std::string& src, const std::string& dst,
                                 bool cow) {
    std::error_code ec;

    if (cow) {
        // Try copy-on-write first (reflink on supported filesystems)
#if defined(__linux__)
        int src_fd = ::open(src.c_str(), O_RDONLY);
        if (src_fd >= 0) {
            int dst_fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (dst_fd >= 0) {
                if (::ioctl(dst_fd, FICLONE, src_fd) == 0) {
                    ::close(dst_fd);
                    ::close(src_fd);
                    return true;
                }

                ::close(dst_fd);
                fs::remove(dst, ec);
                ec.clear();
            }
            ::close(src_fd);
        }
#elif defined(__APPLE__)
        if (::clonefile(src.c_str(), dst.c_str(), 0) == 0) {
            return true;
        }

        if (errno != ENOTSUP && errno != EXDEV) {
            fs::remove(dst, ec);
            ec.clear();
        }
#endif
    }

    // Regular copy
    auto opts = fs::copy_options::overwrite_existing;
    fs::copy_file(src, dst, opts, ec);
    return !ec;
}

// ============================================================================
// Internal lookup helpers
// ============================================================================

InstanceInfo* InstanceManager::find_instance(const std::string& id) {
    auto it = instances_.find(id);
    return (it != instances_.end()) ? &it->second : nullptr;
}

const InstanceInfo* InstanceManager::find_instance(const std::string& id) const {
    auto it = instances_.find(id);
    return (it != instances_.end()) ? &it->second : nullptr;
}

const InstanceInfo* InstanceManager::find_by_name(const std::string& name) const {
    for (const auto& [id, info] : instances_) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

// ============================================================================
// Instance lifecycle
// ============================================================================

rex::hal::HalResult<std::string> InstanceManager::create_instance(
    const InstanceCreateConfig& config)
{
    std::lock_guard lock(mutex_);

    // Check instance limit
    if (instances_.size() >= MAX_INSTANCES) {
        fprintf(stderr, "InstanceManager: max instances (%u) reached\n", MAX_INSTANCES);
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }

    // Check name uniqueness
    if (config.name.empty()) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }
    if (find_by_name(config.name) != nullptr) {
        fprintf(stderr, "InstanceManager: instance name '%s' already exists\n",
                config.name.c_str());
        return std::unexpected(rex::hal::HalError::AlreadyExists);
    }

    // Generate ID and create instance
    std::string id = generate_id();

    InstanceInfo info{};
    info.id = id;
    info.name = config.name;
    info.state = InstanceState::Created;
    info.config_path = config.config_path;
    info.ram_mb = (config.ram_mb > 0) ? config.ram_mb : 2048;
    info.vcpu_count = (config.vcpu_count > 0) ? config.vcpu_count : 2;
    info.system_image = config.system_image;
    info.data_image = config.data_image;
    info.created_at = current_timestamp();

    // Create instance data directory
    std::string instance_dir = data_dir_ + "/" + id;
    std::error_code ec;
    fs::create_directories(instance_dir, ec);
    if (ec) {
        fprintf(stderr, "InstanceManager: failed to create dir '%s': %s\n",
                instance_dir.c_str(), ec.message().c_str());
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    instances_[id] = std::move(info);
    return id;
}

rex::hal::HalResult<void> InstanceManager::destroy_instance(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto* info = find_instance(id);
    if (!info) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    // Must be stopped or created (not running/paused)
    if (info->state == InstanceState::Running ||
        info->state == InstanceState::Paused ||
        info->state == InstanceState::Starting) {
        fprintf(stderr, "InstanceManager: cannot destroy instance '%s' in state %s\n",
                info->name.c_str(), instance_state_str(info->state));
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // Remove instance data directory
    std::string instance_dir = data_dir_ + "/" + id;
    std::error_code ec;
    fs::remove_all(instance_dir, ec);
    // Ignore removal errors (directory may not exist)

    instances_.erase(id);
    return {};
}

rex::hal::HalResult<void> InstanceManager::start_instance(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto* info = find_instance(id);
    if (!info) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    if (info->state != InstanceState::Created &&
        info->state != InstanceState::Stopped) {
        fprintf(stderr, "InstanceManager: cannot start instance '%s' from state %s\n",
                info->name.c_str(), instance_state_str(info->state));
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    info->state = InstanceState::Starting;
    info->last_started = current_timestamp();

    // In a full implementation, this would:
    // 1. Load the VmCreateConfig from info->config_path
    // 2. Create a Vm instance
    // 3. Call vm.create(config)
    // 4. Call vm.start()
    // The actual VM object would be stored in a separate map.

    info->state = InstanceState::Running;
    return {};
}

rex::hal::HalResult<void> InstanceManager::stop_instance(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto* info = find_instance(id);
    if (!info) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    if (info->state != InstanceState::Running &&
        info->state != InstanceState::Paused) {
        fprintf(stderr, "InstanceManager: cannot stop instance '%s' from state %s\n",
                info->name.c_str(), instance_state_str(info->state));
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    info->state = InstanceState::Stopping;

    // In a full implementation, this would:
    // 1. Call vm.stop() on the associated Vm object
    // 2. Wait for vCPU threads to join
    // 3. Clean up resources

    info->state = InstanceState::Stopped;
    return {};
}

rex::hal::HalResult<void> InstanceManager::pause_instance(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto* info = find_instance(id);
    if (!info) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    if (info->state != InstanceState::Running) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    info->state = InstanceState::Paused;
    return {};
}

rex::hal::HalResult<void> InstanceManager::resume_instance(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto* info = find_instance(id);
    if (!info) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    if (info->state != InstanceState::Paused) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    info->state = InstanceState::Running;
    return {};
}

// ============================================================================
// Query
// ============================================================================

std::vector<InstanceInfo> InstanceManager::list_instances() const {
    std::lock_guard lock(mutex_);

    std::vector<InstanceInfo> result;
    result.reserve(instances_.size());

    for (const auto& [id, info] : instances_) {
        result.push_back(info);
    }

    // Sort by creation time
    std::sort(result.begin(), result.end(),
              [](const InstanceInfo& a, const InstanceInfo& b) {
                  return a.created_at < b.created_at;
              });

    return result;
}

rex::hal::HalResult<InstanceInfo> InstanceManager::get_instance(
    const std::string& id) const
{
    std::lock_guard lock(mutex_);

    const auto* info = find_instance(id);
    if (!info) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    return *info;
}

uint32_t InstanceManager::active_count() const {
    std::lock_guard lock(mutex_);
    uint32_t count = 0;
    for (const auto& [id, info] : instances_) {
        if (info.state == InstanceState::Running ||
            info.state == InstanceState::Paused ||
            info.state == InstanceState::Starting) {
            ++count;
        }
    }
    return count;
}

uint32_t InstanceManager::total_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<uint32_t>(instances_.size());
}

// ============================================================================
// Cloning
// ============================================================================

rex::hal::HalResult<std::string> InstanceManager::clone_instance(
    const std::string& source_id,
    const CloneOptions& options)
{
    std::lock_guard lock(mutex_);

    // Validate source
    const auto* source = find_instance(source_id);
    if (!source) {
        return std::unexpected(rex::hal::HalError::DeviceNotFound);
    }

    // Source should be stopped or created for consistent clone
    if (source->state == InstanceState::Running ||
        source->state == InstanceState::Starting) {
        fprintf(stderr,
                "InstanceManager: cannot clone running instance '%s'; stop it first\n",
                source->name.c_str());
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // Check instance limit
    if (instances_.size() >= MAX_INSTANCES) {
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }

    // Check name uniqueness
    if (options.new_name.empty()) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }
    if (find_by_name(options.new_name) != nullptr) {
        return std::unexpected(rex::hal::HalError::AlreadyExists);
    }

    // Generate new ID
    std::string new_id = generate_id();

    // Create new instance directory
    std::string new_dir = data_dir_ + "/" + new_id;
    std::error_code ec;
    fs::create_directories(new_dir, ec);
    if (ec) {
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    // Clone disk images
    if (!source->system_image.empty()) {
        std::string src_img = source->system_image;
        std::string dst_img = new_dir + "/" + fs::path(src_img).filename().string();
        if (fs::exists(src_img)) {
            if (!copy_file(src_img, dst_img, options.copy_on_write)) {
                fprintf(stderr,
                        "InstanceManager: failed to clone system image '%s'\n",
                        src_img.c_str());
                fs::remove_all(new_dir, ec);
                return std::unexpected(rex::hal::HalError::InternalError);
            }
        }
    }

    if (options.clone_data_image && !source->data_image.empty()) {
        std::string src_img = source->data_image;
        std::string dst_img = new_dir + "/" + fs::path(src_img).filename().string();
        if (fs::exists(src_img)) {
            if (!copy_file(src_img, dst_img, options.copy_on_write)) {
                fprintf(stderr,
                        "InstanceManager: failed to clone data image '%s'\n",
                        src_img.c_str());
                fs::remove_all(new_dir, ec);
                return std::unexpected(rex::hal::HalError::InternalError);
            }
        }
    }

    // Create the new InstanceInfo
    InstanceInfo info{};
    info.id = new_id;
    info.name = options.new_name;
    info.state = InstanceState::Created;
    info.config_path = source->config_path;
    info.ram_mb = source->ram_mb;
    info.vcpu_count = source->vcpu_count;
    info.created_at = current_timestamp();

    // Update image paths to point to cloned copies
    if (!source->system_image.empty()) {
        info.system_image = new_dir + "/" +
            fs::path(source->system_image).filename().string();
    }
    if (options.clone_data_image && !source->data_image.empty()) {
        info.data_image = new_dir + "/" +
            fs::path(source->data_image).filename().string();
    }

    instances_[new_id] = std::move(info);
    return new_id;
}

// ============================================================================
// Persistence (JSON)
// ============================================================================

rex::hal::HalResult<void> InstanceManager::save_state(const std::string& path) const {
    std::lock_guard lock(mutex_);

    // Simple JSON serialization (no external dependency)
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        fprintf(stderr, "InstanceManager: failed to open '%s' for writing\n",
                path.c_str());
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    auto escape_json = [](const std::string& s) -> std::string {
        std::string result;
        result.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n";  break;
                case '\r': result += "\\r";  break;
                case '\t': result += "\\t";  break;
                default:   result += c;      break;
            }
        }
        return result;
    };

    out << "{\n  \"version\": 1,\n  \"instances\": [\n";

    bool first = true;
    for (const auto& [id, info] : instances_) {
        if (!first) out << ",\n";
        first = false;

        out << "    {\n"
            << "      \"id\": \"" << escape_json(info.id) << "\",\n"
            << "      \"name\": \"" << escape_json(info.name) << "\",\n"
            << "      \"state\": \"" << instance_state_str(info.state) << "\",\n"
            << "      \"config_path\": \"" << escape_json(info.config_path) << "\",\n"
            << "      \"ram_mb\": " << info.ram_mb << ",\n"
            << "      \"vcpu_count\": " << info.vcpu_count << ",\n"
            << "      \"system_image\": \"" << escape_json(info.system_image) << "\",\n"
            << "      \"data_image\": \"" << escape_json(info.data_image) << "\",\n"
            << "      \"created_at\": \"" << escape_json(info.created_at) << "\",\n"
            << "      \"last_started\": \"" << escape_json(info.last_started) << "\"\n"
            << "    }";
    }

    out << "\n  ]\n}\n";

    if (!out.good()) {
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    return {};
}

rex::hal::HalResult<void> InstanceManager::load_state(const std::string& path) {
    std::lock_guard lock(mutex_);

    std::ifstream in(path);
    if (!in) {
        // File doesn't exist = empty state, not an error
        return {};
    }

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    // Minimal JSON parser for our known format
    // This is intentionally simple — production code would use nlohmann/json or similar.
    // We parse the "instances" array by finding each { ... } block.

    auto find_string_value = [&](const std::string& block,
                                  const std::string& key) -> std::string {
        std::string search = "\"" + key + "\": \"";
        auto pos = block.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = block.find('"', pos);
        if (end == std::string::npos) return "";
        return block.substr(pos, end - pos);
    };

    auto find_uint_value = [&](const std::string& block,
                                const std::string& key) -> uint64_t {
        std::string search = "\"" + key + "\": ";
        auto pos = block.find(search);
        if (pos == std::string::npos) return 0;
        pos += search.size();
        return std::stoull(block.substr(pos));
    };

    // Find each instance object in the array
    instances_.clear();

    size_t search_start = content.find("\"instances\"");
    if (search_start == std::string::npos) return {};

    size_t pos = search_start;
    while (true) {
        size_t obj_start = content.find('{', pos + 1);
        if (obj_start == std::string::npos) break;

        // Don't go past the end of the instances array
        size_t array_end = content.find(']', search_start);
        if (array_end != std::string::npos && obj_start > array_end) break;

        size_t obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string block = content.substr(obj_start, obj_end - obj_start + 1);

        InstanceInfo info{};
        info.id = find_string_value(block, "id");
        info.name = find_string_value(block, "name");
        info.config_path = find_string_value(block, "config_path");
        info.ram_mb = find_uint_value(block, "ram_mb");
        info.vcpu_count = static_cast<uint32_t>(find_uint_value(block, "vcpu_count"));
        info.system_image = find_string_value(block, "system_image");
        info.data_image = find_string_value(block, "data_image");
        info.created_at = find_string_value(block, "created_at");
        info.last_started = find_string_value(block, "last_started");

        // Parse state — all loaded instances start as Stopped
        // (they weren't running when we saved)
        std::string state_str = find_string_value(block, "state");
        if (state_str == "Created") info.state = InstanceState::Created;
        else info.state = InstanceState::Stopped;

        if (!info.id.empty()) {
            instances_[info.id] = std::move(info);
        }

        pos = obj_end;
    }

    return {};
}

// ============================================================================
// Data directory
// ============================================================================

void InstanceManager::set_data_dir(const std::string& path) {
    std::lock_guard lock(mutex_);
    data_dir_ = path;

    // Ensure directory exists
    std::error_code ec;
    fs::create_directories(data_dir_, ec);
}

} // namespace rex::vmm
