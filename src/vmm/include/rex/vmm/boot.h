#pragma once

#include "rex/hal/hypervisor.h"
#include "rex/vmm/memory_manager.h"
#include <string>
#include <vector>

namespace rex::vmm {

/// Boot parameters for direct kernel boot (Linux boot protocol)
struct BootParams {
    std::string kernel_path;       // Path to bzImage / Image
    std::string initrd_path;       // Path to initrd/initramfs (optional)
    std::string cmdline;           // Kernel command line
};

/// Load and set up a Linux kernel for direct boot (x86_64)
///
/// Implements the Linux x86 boot protocol:
/// - Loads bzImage at appropriate GPA
/// - Sets up boot_params struct at 0x7000
/// - Loads initrd above the kernel
/// - Configures vCPU registers for 64-bit entry
rex::hal::HalResult<void> setup_direct_boot_x86(
    rex::hal::IVcpu& vcpu,
    MemoryManager& mem,
    const BootParams& params
);

/// Load and set up a Linux kernel for direct boot (ARM64)
///
/// Implements the ARM64 boot protocol (Documentation/arm64/booting.rst):
/// - Loads Image at RAM_BASE + 0x80000
/// - Loads DTB at RAM_BASE + 64MB
/// - Loads initrd at RAM_BASE + 128MB
/// - Sets x0 = DTB address, PC = kernel entry
rex::hal::HalResult<void> setup_direct_boot_arm64(
    rex::hal::IVcpu& vcpu,
    MemoryManager& mem,
    const BootParams& params
);

} // namespace rex::vmm
