#pragma once

#include "rex/hal/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rex::vmm {

class Vm;

/// Save and restore VM state (vCPU registers, RAM, device state)
class SnapshotManager {
public:
    /// Save the entire VM state to a file
    static rex::hal::HalResult<void> save(const Vm& vm, const std::string& path);

    /// Restore a VM from a saved snapshot
    static rex::hal::HalResult<void> restore(Vm& vm, const std::string& path);

    /// RLE-compress data (optimized for runs of zeros common in VM memory)
    ///
    /// Format:
    ///   [count:u32][byte:u8]          — run of `count` identical bytes
    ///   [0:u32][count:u32][data...]   — literal run of `count` non-repeating bytes
    static std::vector<uint8_t> rle_compress(const uint8_t* data, size_t size);

    /// Decompress RLE-encoded data back to original
    static std::vector<uint8_t> rle_decompress(const uint8_t* data, size_t size,
                                                size_t expected_size);
};

} // namespace rex::vmm
