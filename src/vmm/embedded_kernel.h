#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace rex::vmm {

/// Generates a minimal ARM64 test kernel that:
/// 1. Writes "RexPlayer" to MMIO UART at 0x09000000
/// 2. Enters a WFI loop
///
/// This is used as a fallback when no kernel image is provided,
/// allowing the VMM to verify that the hypervisor, vCPU, memory
/// mapping, and MMIO exit handling are all working.
std::vector<uint8_t> generate_test_kernel_arm64();

/// Generates a minimal x86_64 test kernel that:
/// 1. Writes "REX" to I/O port 0x3F8 (COM1 UART)
/// 2. Executes HLT
std::vector<uint8_t> generate_test_kernel_x86();

/// Save a kernel blob to a temp file and return the path.
/// The file is created in the system temp directory.
std::string save_temp_kernel(const std::vector<uint8_t>& kernel,
                              const std::string& name);

} // namespace rex::vmm
