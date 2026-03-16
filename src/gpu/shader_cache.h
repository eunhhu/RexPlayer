#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::gpu {

/// GPU shader cache for virglrenderer
///
/// Caches compiled shader programs on disk to avoid recompilation
/// on subsequent launches. The cache key is a hash of the shader
/// source (TGSI/GLSL) and the cache value is the compiled binary.
class ShaderCache {
public:
    /// Create a shader cache at the given directory
    explicit ShaderCache(const std::filesystem::path& cache_dir);

    /// Look up a cached shader by its source hash
    /// Returns empty vector if not found
    std::vector<uint8_t> lookup(uint64_t hash) const;

    /// Store a compiled shader in the cache
    void store(uint64_t hash, const uint8_t* data, size_t size);

    /// Check if a shader is in the cache
    bool contains(uint64_t hash) const;

    /// Clear all cached shaders
    void clear();

    /// Get the number of cached entries
    size_t size() const;

    /// Get total cache size in bytes
    size_t total_bytes() const;

    /// Load cache index from disk
    bool load_index();

    /// Compute a hash for shader source data
    static uint64_t hash_shader(const uint8_t* data, size_t size);

private:
    std::filesystem::path cache_dir_;
    mutable std::mutex mutex_;

    struct CacheEntry {
        uint64_t hash;
        size_t size;
        std::filesystem::path path;
    };

    std::unordered_map<uint64_t, CacheEntry> index_;
};

} // namespace rex::gpu
