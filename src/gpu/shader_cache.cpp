#include "shader_cache.h"
#include <cstring>
#include <fstream>

namespace rex::gpu {

ShaderCache::ShaderCache(const std::filesystem::path& cache_dir)
    : cache_dir_(cache_dir) {
    std::filesystem::create_directories(cache_dir_);
}

std::vector<uint8_t> ShaderCache::lookup(uint64_t hash) const {
    std::lock_guard lock(mutex_);

    auto it = index_.find(hash);
    if (it == index_.end()) return {};

    std::ifstream file(it->second.path, std::ios::binary);
    if (!file.is_open()) return {};

    std::vector<uint8_t> data(it->second.size);
    file.read(reinterpret_cast<char*>(data.data()),
              static_cast<std::streamsize>(data.size()));

    if (!file) return {};
    return data;
}

void ShaderCache::store(uint64_t hash, const uint8_t* data, size_t size) {
    std::lock_guard lock(mutex_);

    // Build filename from hash
    char filename[32];
    snprintf(filename, sizeof(filename), "%016llx.bin",
             static_cast<unsigned long long>(hash));

    auto path = cache_dir_ / filename;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return;

    file.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(size));

    if (file) {
        index_[hash] = CacheEntry{hash, size, path};
    }
}

bool ShaderCache::contains(uint64_t hash) const {
    std::lock_guard lock(mutex_);
    return index_.count(hash) > 0;
}

void ShaderCache::clear() {
    std::lock_guard lock(mutex_);

    for (auto& [_, entry] : index_) {
        std::filesystem::remove(entry.path);
    }
    index_.clear();
}

size_t ShaderCache::size() const {
    std::lock_guard lock(mutex_);
    return index_.size();
}

size_t ShaderCache::total_bytes() const {
    std::lock_guard lock(mutex_);
    size_t total = 0;
    for (const auto& [_, entry] : index_) {
        total += entry.size;
    }
    return total;
}

bool ShaderCache::load_index() {
    std::lock_guard lock(mutex_);

    if (!std::filesystem::exists(cache_dir_)) return false;

    for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".bin") continue;

        // Parse hash from filename
        std::string stem = entry.path().stem().string();
        if (stem.size() != 16) continue;

        uint64_t hash = 0;
        try {
            hash = std::stoull(stem, nullptr, 16);
        } catch (...) {
            continue;
        }

        index_[hash] = CacheEntry{
            hash,
            static_cast<size_t>(entry.file_size()),
            entry.path()
        };
    }

    return true;
}

uint64_t ShaderCache::hash_shader(const uint8_t* data, size_t size) {
    // FNV-1a 64-bit hash
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

} // namespace rex::gpu
