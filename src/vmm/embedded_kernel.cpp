#include "embedded_kernel.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace rex::vmm {

std::vector<uint8_t> generate_test_kernel_arm64() {
    // ARM64 instructions (little-endian uint32_t)
    // This mini kernel writes "RexPlayer\n" to MMIO UART at 0x09000000
    // then enters a WFI loop.

    std::vector<uint32_t> code;

    // Load UART base address (0x09000000) into X1
    // MOVZ X1, #0x0900, LSL #16
    code.push_back(0xD2A12001);
    // MOVK X1, #0x0000
    code.push_back(0xF2800001);

    // Write "RexPlayer\n" character by character
    const char* msg = "RexPlayer\n";
    for (size_t i = 0; i < strlen(msg); ++i) {
        // MOVZ X0, #<char>
        uint32_t mov = 0xD2800000 | (static_cast<uint32_t>(msg[i]) << 5);
        code.push_back(mov);
        // STRB W0, [X1]
        code.push_back(0x39000020);
    }

    // Infinite WFI loop
    // wfi_loop:
    code.push_back(0xD503207F);  // WFI
    // B wfi_loop (branch to self: offset = -4 bytes = -1 instruction)
    code.push_back(0x17FFFFFF);  // B #-4

    // Convert to byte array
    std::vector<uint8_t> kernel;
    kernel.resize(code.size() * 4);
    for (size_t i = 0; i < code.size(); ++i) {
        uint32_t insn = code[i];
        kernel[i * 4 + 0] = static_cast<uint8_t>(insn & 0xFF);
        kernel[i * 4 + 1] = static_cast<uint8_t>((insn >> 8) & 0xFF);
        kernel[i * 4 + 2] = static_cast<uint8_t>((insn >> 16) & 0xFF);
        kernel[i * 4 + 3] = static_cast<uint8_t>((insn >> 24) & 0xFF);
    }

    // Pad to 4KB minimum
    if (kernel.size() < 4096) {
        kernel.resize(4096, 0);
    }

    return kernel;
}

std::vector<uint8_t> generate_test_kernel_x86() {
    // x86 real-mode code that writes "REX" to COM1 (port 0x3F8) then HLTs
    std::vector<uint8_t> kernel;

    auto emit = [&kernel](std::initializer_list<uint8_t> bytes) {
        kernel.insert(kernel.end(), bytes);
    };

    // mov dx, 0x3F8
    emit({0xBA, 0xF8, 0x03});
    // mov al, 'R'
    emit({0xB0, 0x52});
    // out dx, al
    emit({0xEE});
    // mov al, 'E'
    emit({0xB0, 0x45});
    // out dx, al
    emit({0xEE});
    // mov al, 'X'
    emit({0xB0, 0x58});
    // out dx, al
    emit({0xEE});
    // mov al, '\n'
    emit({0xB0, 0x0A});
    // out dx, al
    emit({0xEE});
    // hlt
    emit({0xF4});
    // jmp to hlt (infinite loop)
    emit({0xEB, 0xFD});

    // Pad to 4KB
    kernel.resize(4096, 0);

    return kernel;
}

std::string save_temp_kernel(const std::vector<uint8_t>& kernel,
                              const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() / "rexplayer";
    std::filesystem::create_directories(dir);

    auto path = dir / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return {};

    file.write(reinterpret_cast<const char*>(kernel.data()),
               static_cast<std::streamsize>(kernel.size()));
    file.close();

    return path.string();
}

} // namespace rex::vmm
