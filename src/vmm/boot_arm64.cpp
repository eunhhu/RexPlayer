#include "rex/vmm/boot.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace rex::vmm {

// ARM64 boot protocol constants
// The Linux ARM64 boot protocol (Documentation/arm64/booting.rst) expects:
// - Kernel Image loaded at a 2MB-aligned address (typically RAM base + 0x80000)
// - Device tree blob (DTB) at a 64-byte aligned address
// - x0 = DTB physical address, x1-x3 = 0

static constexpr rex::hal::GPA RAM_BASE          = 0x40000000; // 1 GB
static constexpr rex::hal::GPA KERNEL_LOAD_ADDR  = 0x40080000; // RAM + 512KB
static constexpr rex::hal::GPA DTB_LOAD_ADDR     = 0x44000000; // RAM + 64MB
static constexpr rex::hal::GPA INITRD_LOAD_ADDR  = 0x48000000; // RAM + 128MB

// ARM64 Image header magic
static constexpr uint32_t ARM64_MAGIC = 0x644D5241; // "ARM\x64"
static constexpr size_t   ARM64_MAGIC_OFFSET = 56;

/// Read a file into a byte vector
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

/// Generate a minimal device tree blob for RexPlayer ARM64 guest
///
/// This creates a FDT with:
/// - /chosen: bootargs, initrd-start/end
/// - /memory: ram base and size
/// - /cpus: one cpu node
/// - /psci: power state coordination interface
static std::vector<uint8_t> generate_minimal_dtb(
    rex::hal::GPA ram_base,
    rex::hal::MemSize ram_size,
    const std::string& cmdline,
    rex::hal::GPA initrd_start,
    rex::hal::GPA initrd_end)
{
    // FDT header constants
    static constexpr uint32_t FDT_MAGIC     = 0xD00DFEED;
    static constexpr uint32_t FDT_BEGIN_NODE = 0x00000001;
    static constexpr uint32_t FDT_END_NODE   = 0x00000002;
    static constexpr uint32_t FDT_PROP       = 0x00000003;
    static constexpr uint32_t FDT_END        = 0x00000009;

    // For a production DTB we'd use libfdt, but for bootstrapping we generate
    // a minimal one. This is a placeholder that will be replaced with proper
    // DTB generation using libfdt in Phase 2.
    //
    // For now, return an empty vector — the actual DTB will be loaded from
    // a pre-built file supplied alongside the kernel.
    (void)ram_base;
    (void)ram_size;
    (void)cmdline;
    (void)initrd_start;
    (void)initrd_end;
    (void)FDT_MAGIC;
    (void)FDT_BEGIN_NODE;
    (void)FDT_END_NODE;
    (void)FDT_PROP;
    (void)FDT_END;

    return {};
}

rex::hal::HalResult<void> setup_direct_boot_arm64(
    rex::hal::IVcpu& vcpu,
    MemoryManager& mem,
    const BootParams& params)
{
    // 1. Read the kernel Image
    auto kernel = read_file(params.kernel_path);
    if (kernel.empty()) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // 2. Verify ARM64 Image magic
    if (kernel.size() < ARM64_MAGIC_OFFSET + 4) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    uint32_t magic = 0;
    memcpy(&magic, &kernel[ARM64_MAGIC_OFFSET], 4);
    if (magic != ARM64_MAGIC) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // 3. Load kernel at KERNEL_LOAD_ADDR
    auto write_result = mem.write(KERNEL_LOAD_ADDR, kernel.data(), kernel.size());
    if (!write_result) return write_result;

    // 4. Load initrd if specified
    rex::hal::GPA initrd_start = 0;
    rex::hal::GPA initrd_end = 0;
    if (!params.initrd_path.empty()) {
        auto initrd = read_file(params.initrd_path);
        if (!initrd.empty()) {
            initrd_start = INITRD_LOAD_ADDR;
            initrd_end = INITRD_LOAD_ADDR + initrd.size();
            auto initrd_result = mem.write(INITRD_LOAD_ADDR, initrd.data(), initrd.size());
            if (!initrd_result) return initrd_result;
        }
    }

    // 5. Load or generate DTB
    // For now, we expect a pre-built DTB. In Phase 2, we'll generate one.
    // The DTB path can be passed via cmdline or a separate field.
    // Placeholder: write an empty DTB region
    std::vector<uint8_t> dtb(4096, 0);
    auto dtb_result = mem.write(DTB_LOAD_ADDR, dtb.data(), dtb.size());
    if (!dtb_result) return dtb_result;

    // 6. Set up vCPU registers
    // ARM64 boot protocol:
    //   x0 = physical address of DTB
    //   x1 = 0 (reserved)
    //   x2 = 0 (reserved)
    //   x3 = 0 (reserved)
    //   PC = kernel entry point
    //
    // Note: ARM64 registers are set differently than x86.
    // The IVcpu interface currently defines x86 registers only.
    // ARM64 support will be added to the IVcpu interface in Phase 4
    // when we implement the HVF ARM64 backend for Apple Silicon.
    //
    // For now, this function prepares the memory layout.
    // The actual register setup will be done by the ARM64-specific vCPU.

    (void)vcpu; // Will be used when ARM64 IVcpu is implemented

    return {};
}

} // namespace rex::vmm
