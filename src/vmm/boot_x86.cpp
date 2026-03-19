#include "rex/vmm/boot.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace rex::vmm {

// Linux x86 boot protocol constants
static constexpr rex::hal::GPA BOOT_PARAMS_ADDR  = 0x7000;
static constexpr rex::hal::GPA CMDLINE_ADDR      = 0x20000;
static constexpr rex::hal::GPA KERNEL_LOAD_ADDR  = 0x100000;  // 1 MB — protected mode kernel
static constexpr rex::hal::GPA INITRD_LOAD_ADDR  = 0x4000000; // 64 MB

// Linux boot_params header offsets (from Documentation/x86/boot.rst)
static constexpr size_t HEADER_OFFSET       = 0x1F1;
static constexpr size_t SETUP_SECTS_OFFSET  = 0x1F1;
static constexpr size_t BOOT_FLAG_OFFSET    = 0x1FE;
static constexpr size_t HEADER_MAGIC_OFFSET = 0x202;
static constexpr size_t TYPE_OF_LOADER      = 0x210;
static constexpr size_t LOADFLAGS_OFFSET    = 0x211;
static constexpr size_t RAMDISK_IMAGE       = 0x218;
static constexpr size_t RAMDISK_SIZE        = 0x21C;
static constexpr size_t CMD_LINE_PTR        = 0x228;

static constexpr uint32_t HEADER_MAGIC = 0x53726448; // "HdrS"
static constexpr uint16_t BOOT_FLAG    = 0xAA55;

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

rex::hal::HalResult<void> setup_direct_boot_x86(
    rex::hal::IVcpu& vcpu,
    MemoryManager& mem,
    const BootParams& params)
{
    // 1. Read the kernel image (bzImage)
    auto kernel = read_file(params.kernel_path);
    if (kernel.empty()) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // 2. Parse the setup header
    if (kernel.size() < 0x250) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    uint32_t magic = 0;
    memcpy(&magic, &kernel[HEADER_MAGIC_OFFSET], 4);
    if (magic != HEADER_MAGIC) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    // Number of setup sectors (real-mode code)
    uint8_t setup_sects = kernel[SETUP_SECTS_OFFSET];
    if (setup_sects == 0) setup_sects = 4; // default per protocol

    size_t setup_size = (setup_sects + 1) * 512;
    size_t kernel_size = kernel.size() - setup_size;

    // 3. Prepare boot_params at BOOT_PARAMS_ADDR
    //    Copy the setup header (first setup_size bytes of bzImage)
    std::vector<uint8_t> boot_params(4096, 0);
    if (setup_size <= boot_params.size()) {
        memcpy(boot_params.data(), kernel.data(), setup_size);
    }

    // Set type_of_loader (0xFF = undefined)
    boot_params[TYPE_OF_LOADER] = 0xFF;

    // Set load flags: LOADED_HIGH (bit 0) — kernel loaded at 1MB
    boot_params[LOADFLAGS_OFFSET] |= 0x01;

    // Kernel command line
    if (!params.cmdline.empty()) {
        uint32_t cmdline_addr = static_cast<uint32_t>(CMDLINE_ADDR);
        memcpy(&boot_params[CMD_LINE_PTR], &cmdline_addr, 4);

        mem.write(CMDLINE_ADDR, params.cmdline.c_str(), params.cmdline.size() + 1);
    }

    // 4. Load initrd if specified
    if (!params.initrd_path.empty()) {
        auto initrd = read_file(params.initrd_path);
        if (!initrd.empty()) {
            uint32_t initrd_addr = static_cast<uint32_t>(INITRD_LOAD_ADDR);
            uint32_t initrd_size = static_cast<uint32_t>(initrd.size());
            memcpy(&boot_params[RAMDISK_IMAGE], &initrd_addr, 4);
            memcpy(&boot_params[RAMDISK_SIZE], &initrd_size, 4);

            mem.write(INITRD_LOAD_ADDR, initrd.data(), initrd.size());
        }
    }

    // 5. Write boot_params to guest memory
    mem.write(BOOT_PARAMS_ADDR, boot_params.data(), boot_params.size());

    // 6. Load the protected-mode kernel at 1 MB
    mem.write(KERNEL_LOAD_ADDR, kernel.data() + setup_size, kernel_size);

    // 7. Set up vCPU registers for 64-bit long mode entry
    //    The kernel's 64-bit entry point expects:
    //    - RSI = pointer to boot_params
    //    - RIP = KERNEL_LOAD_ADDR (start of protected-mode code)
    //    - CS in 64-bit mode with proper GDT

    // Set up general registers
    rex::hal::X86Regs regs{};
    regs.rsi = BOOT_PARAMS_ADDR;
    regs.rip = KERNEL_LOAD_ADDR;
    regs.rflags = 0x2; // Reserved bit must be set

    auto regs_result = vcpu.set_regs(regs);
    if (!regs_result) return regs_result;

    // Set up segment registers for protected mode (the kernel will switch to long mode)
    auto sregs_result = vcpu.get_sregs();
    if (!sregs_result) return std::unexpected(sregs_result.error());

    auto sregs = *sregs_result;

    // Enable protected mode
    sregs.cr0 = 0x1; // PE bit
    sregs.cr4 = 0;

    // Flat code segment
    sregs.cs.base = 0;
    sregs.cs.limit = 0xFFFFFFFF;
    sregs.cs.selector = 0x10;
    sregs.cs.type = 0xB; // Execute/Read, accessed
    sregs.cs.present = 1;
    sregs.cs.dpl = 0;
    sregs.cs.db = 1;
    sregs.cs.s = 1;
    sregs.cs.l = 0;
    sregs.cs.g = 1;

    // Flat data segment
    auto setup_data_seg = [](rex::hal::X86Segment& seg) {
        seg.base = 0;
        seg.limit = 0xFFFFFFFF;
        seg.selector = 0x18;
        seg.type = 0x3; // Read/Write, accessed
        seg.present = 1;
        seg.dpl = 0;
        seg.db = 1;
        seg.s = 1;
        seg.l = 0;
        seg.g = 1;
    };

    setup_data_seg(sregs.ds);
    setup_data_seg(sregs.es);
    setup_data_seg(sregs.fs);
    setup_data_seg(sregs.gs);
    setup_data_seg(sregs.ss);

    auto sregs_set_result = vcpu.set_sregs(sregs);
    if (!sregs_set_result) return sregs_set_result;

    return {};
}

} // namespace rex::vmm
