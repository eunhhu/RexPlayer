#include "rex/vmm/snapshot.h"
#include "rex/vmm/vm.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace rex::vmm {

// ---------------------------------------------------------------------------
// Snapshot file format
// ---------------------------------------------------------------------------
// [SnapshotHeader]          — magic, version, timestamp, layout offsets
// [VcpuSnapshot × N]       — X86Regs + X86Sregs for each vCPU
// [CompressedRAM]           — RLE-compressed guest memory
// [DeviceState] (reserved)  — placeholder for future device serialization
// ---------------------------------------------------------------------------

static constexpr uint32_t SNAPSHOT_MAGIC   = 0x52455853; // "REXS"
static constexpr uint32_t SNAPSHOT_VERSION = 1;

/// Minimum run length before we encode as a run (tuning knob)
static constexpr uint32_t MIN_RUN_LENGTH = 4;

/// Maximum literal run before we flush
static constexpr uint32_t MAX_LITERAL_RUN = 65536;

#pragma pack(push, 1)
struct SnapshotHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp;       // milliseconds since epoch
    uint32_t num_vcpus;
    uint64_t ram_size;
    uint64_t ram_offset;      // file offset where compressed RAM starts
    uint64_t device_offset;   // file offset where device state starts (0 = none)
    uint64_t compressed_size; // size of compressed RAM data
};

struct VcpuSnapshot {
    rex::hal::X86Regs regs;
    rex::hal::X86Sregs sregs;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// RLE compression
// ---------------------------------------------------------------------------
// Encoding:
//   Run:     [count:u32][byte:u8]                — `count` copies of `byte`
//   Literal: [0x00000000:u32][count:u32][data…]  — `count` raw bytes follow
//
// Runs of identical bytes (especially zeros) dominate VM memory, so runs
// are the common case and literals handle the rest.
// ---------------------------------------------------------------------------

static void write_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> SnapshotManager::rle_compress(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    // Reserve a rough estimate — compressed VM memory is typically much smaller
    out.reserve(size / 4);

    size_t i = 0;
    while (i < size) {
        // Count how many identical bytes follow
        uint8_t cur = data[i];
        size_t run_start = i;
        while (i < size && data[i] == cur && (i - run_start) < 0xFFFFFFFFULL) {
            ++i;
        }
        uint32_t run_len = static_cast<uint32_t>(i - run_start);

        if (run_len >= MIN_RUN_LENGTH) {
            // Encode as a run: [count][byte]
            write_u32(out, run_len);
            out.push_back(cur);
        } else {
            // Too short for a run — collect literals
            // Rewind to start of this short run
            i = run_start;

            // Accumulate literal bytes until we hit a long enough run
            size_t lit_start = i;
            while (i < size && (i - lit_start) < MAX_LITERAL_RUN) {
                // Peek ahead to see if a run is starting
                if (i + MIN_RUN_LENGTH <= size) {
                    uint8_t c = data[i];
                    bool is_run = true;
                    for (uint32_t k = 1; k < MIN_RUN_LENGTH; ++k) {
                        if (data[i + k] != c) {
                            is_run = false;
                            break;
                        }
                    }
                    if (is_run) break; // stop literal, let next iteration handle the run
                }
                ++i;
            }

            uint32_t lit_len = static_cast<uint32_t>(i - lit_start);
            if (lit_len > 0) {
                // Literal marker: [0][count][data...]
                write_u32(out, 0);
                write_u32(out, lit_len);
                out.insert(out.end(), data + lit_start, data + lit_start + lit_len);
            }
        }
    }

    return out;
}

std::vector<uint8_t> SnapshotManager::rle_decompress(const uint8_t* data, size_t size,
                                                      size_t expected_size) {
    std::vector<uint8_t> out;
    out.reserve(expected_size);

    size_t i = 0;
    while (i < size) {
        if (i + 4 > size) break; // truncated

        uint32_t count = read_u32(data + i);
        i += 4;

        if (count == 0) {
            // Literal run: [0][count][data...]
            if (i + 4 > size) break;
            uint32_t lit_len = read_u32(data + i);
            i += 4;

            if (i + lit_len > size) break; // truncated
            out.insert(out.end(), data + i, data + i + lit_len);
            i += lit_len;
        } else {
            // Byte run: [count][byte]
            if (i + 1 > size) break;
            uint8_t byte = data[i];
            ++i;

            // Append `count` copies of `byte`
            out.resize(out.size() + count, byte);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

rex::hal::HalResult<void> SnapshotManager::save(
    const Vm& vm,
    const std::string& path)
{
    // VM must be paused before taking a snapshot
    if (vm.state() != VmState::Paused) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    const auto& config = vm.config();
    const auto& vcpus = vm.vcpus();

    // --- Build header ---
    SnapshotHeader header{};
    header.magic = SNAPSHOT_MAGIC;
    header.version = SNAPSHOT_VERSION;

    auto now = std::chrono::system_clock::now();
    header.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());

    header.num_vcpus = config.num_vcpus;
    header.ram_size = config.ram_size;
    // ram_offset = after header + vcpu snapshots
    header.ram_offset = sizeof(SnapshotHeader)
                      + static_cast<uint64_t>(config.num_vcpus) * sizeof(VcpuSnapshot);
    header.device_offset = 0; // reserved for future
    header.compressed_size = 0; // filled after compression

    // --- Serialize vCPU state ---
    std::vector<VcpuSnapshot> vcpu_snapshots;
    vcpu_snapshots.reserve(config.num_vcpus);

    for (uint32_t i = 0; i < config.num_vcpus; ++i) {
        VcpuSnapshot vs{};

        auto regs_result = vcpus[i]->get_regs();
        if (!regs_result) {
            return std::unexpected(regs_result.error());
        }
        vs.regs = *regs_result;

        auto sregs_result = vcpus[i]->get_sregs();
        if (!sregs_result) {
            return std::unexpected(sregs_result.error());
        }
        vs.sregs = *sregs_result;

        vcpu_snapshots.push_back(vs);
    }

    // --- Compress guest RAM ---
    // Get host pointer for GPA 0 (where RAM is mapped)
    auto host_ptr_result = vm.memory_manager().get_host_ptr(0);
    if (!host_ptr_result) {
        return std::unexpected(host_ptr_result.error());
    }

    const auto* ram_ptr = static_cast<const uint8_t*>(*host_ptr_result);
    auto compressed_ram = rle_compress(ram_ptr, config.ram_size);
    header.compressed_size = compressed_ram.size();

    // Calculate device offset (right after compressed RAM)
    header.device_offset = header.ram_offset + header.compressed_size;

    // --- Write atomically: write to .tmp then rename ---
    std::string tmp_path = path + ".tmp";

    {
        std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return std::unexpected(rex::hal::HalError::InternalError);
        }

        // Write header
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!file.good()) {
            return std::unexpected(rex::hal::HalError::InternalError);
        }

        // Write vCPU snapshots
        for (const auto& vs : vcpu_snapshots) {
            file.write(reinterpret_cast<const char*>(&vs), sizeof(vs));
            if (!file.good()) {
                return std::unexpected(rex::hal::HalError::InternalError);
            }
        }

        // Write compressed RAM
        file.write(reinterpret_cast<const char*>(compressed_ram.data()),
                   static_cast<std::streamsize>(compressed_ram.size()));
        if (!file.good()) {
            return std::unexpected(rex::hal::HalError::InternalError);
        }

        // Flush to disk
        file.flush();
        if (!file.good()) {
            return std::unexpected(rex::hal::HalError::InternalError);
        }
    }

    // Rename .tmp -> final path (atomic on POSIX, best-effort on Windows)
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        // Clean up temp file on failure
        std::filesystem::remove(tmp_path, ec);
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    return {};
}

// ---------------------------------------------------------------------------
// Restore
// ---------------------------------------------------------------------------

rex::hal::HalResult<void> SnapshotManager::restore(
    Vm& vm,
    const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    // --- Read and validate header ---
    SnapshotHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file.good()) {
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    if (header.magic != SNAPSHOT_MAGIC) {
        fprintf(stderr, "snapshot: invalid magic 0x%08X (expected 0x%08X)\n",
                header.magic, SNAPSHOT_MAGIC);
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    if (header.version != SNAPSHOT_VERSION) {
        fprintf(stderr, "snapshot: unsupported version %u (expected %u)\n",
                header.version, SNAPSHOT_VERSION);
        return std::unexpected(rex::hal::HalError::NotSupported);
    }

    if (header.num_vcpus == 0 || header.ram_size == 0) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // --- Create VM with matching configuration ---
    VmCreateConfig config{};
    config.num_vcpus = header.num_vcpus;
    config.ram_size = header.ram_size;
    // Don't set kernel boot params — we're restoring from snapshot

    auto create_result = vm.create(config);
    if (!create_result) {
        return std::unexpected(create_result.error());
    }

    // --- Read vCPU snapshots ---
    std::vector<VcpuSnapshot> vcpu_snapshots(header.num_vcpus);
    for (uint32_t i = 0; i < header.num_vcpus; ++i) {
        file.read(reinterpret_cast<char*>(&vcpu_snapshots[i]), sizeof(VcpuSnapshot));
        if (!file.good()) {
            return std::unexpected(rex::hal::HalError::InternalError);
        }
    }

    // --- Decompress and load RAM ---
    if (header.compressed_size == 0) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    std::vector<uint8_t> compressed_data(header.compressed_size);
    file.seekg(static_cast<std::streamoff>(header.ram_offset));
    file.read(reinterpret_cast<char*>(compressed_data.data()),
              static_cast<std::streamsize>(header.compressed_size));
    if (!file.good()) {
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    auto decompressed = rle_decompress(compressed_data.data(),
                                        compressed_data.size(),
                                        header.ram_size);
    if (decompressed.size() != header.ram_size) {
        fprintf(stderr, "snapshot: RAM decompression size mismatch: got %zu, expected %llu\n",
                decompressed.size(), static_cast<unsigned long long>(header.ram_size));
        return std::unexpected(rex::hal::HalError::InternalError);
    }

    // Write decompressed RAM into guest memory
    auto write_result = vm.memory_manager().write(0, decompressed.data(), decompressed.size());
    if (!write_result) {
        return std::unexpected(write_result.error());
    }

    // --- Restore vCPU registers ---
    auto& vcpus = vm.vcpus();
    for (uint32_t i = 0; i < header.num_vcpus; ++i) {
        auto set_regs_result = vcpus[i]->set_regs(vcpu_snapshots[i].regs);
        if (!set_regs_result) {
            return std::unexpected(set_regs_result.error());
        }

        auto set_sregs_result = vcpus[i]->set_sregs(vcpu_snapshots[i].sregs);
        if (!set_sregs_result) {
            return std::unexpected(set_sregs_result.error());
        }
    }

    // VM is now restored in Paused state — caller can call vm.start() or vm.resume()
    return {};
}

} // namespace rex::vmm
