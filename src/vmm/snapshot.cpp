#include "rex/vmm/snapshot.h"
#include "rex/vmm/vm.h"

#include <cstdint>
#include <cstring>
#include <fstream>

namespace rex::vmm {

// Snapshot file format:
// [Header]  - magic, version, timestamp
// [vCPU 0]  - registers (X86Regs + X86Sregs)
// [vCPU N]  - ...
// [RAM]     - compressed guest memory
// [Devices] - serialized device state (future)

static constexpr uint32_t SNAPSHOT_MAGIC   = 0x52455853; // "REXS"
static constexpr uint32_t SNAPSHOT_VERSION = 1;

#pragma pack(push, 1)
struct SnapshotHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp;
    uint32_t num_vcpus;
    uint64_t ram_size;
    uint64_t ram_offset;     // offset into file where RAM data starts
    uint64_t device_offset;  // offset into file where device state starts
};

struct VcpuSnapshot {
    rex::hal::X86Regs regs;
    rex::hal::X86Sregs sregs;
};
#pragma pack(pop)

rex::hal::HalResult<void> SnapshotManager::save(
    [[maybe_unused]] const Vm& vm,
    [[maybe_unused]] const std::string& path)
{
    // Snapshot saving requires VM to be paused
    // Full implementation will:
    // 1. Pause all vCPUs
    // 2. Serialize vCPU registers
    // 3. Write guest RAM (with optional compression)
    // 4. Serialize device state
    // 5. Write to file atomically (write to .tmp then rename)

    // Placeholder — will be implemented when VM pause/resume is stable
    return std::unexpected(rex::hal::HalError::NotSupported);
}

rex::hal::HalResult<void> SnapshotManager::restore(
    [[maybe_unused]] Vm& vm,
    [[maybe_unused]] const std::string& path)
{
    // Snapshot restoration will:
    // 1. Read and validate header
    // 2. Create VM with matching config
    // 3. Load RAM from snapshot
    // 4. Restore vCPU registers
    // 5. Restore device state
    // 6. Resume VM

    return std::unexpected(rex::hal::HalError::NotSupported);
}

} // namespace rex::vmm
