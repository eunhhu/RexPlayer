#include "rex/vmm/boot.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::vmm {

namespace {

// ARM64 boot protocol constants
// The Linux ARM64 boot protocol (Documentation/arm64/booting.rst) expects:
// - Kernel Image loaded at a 2MB-aligned address (typically RAM base + 0x80000)
// - Device tree blob (DTB) at a 64-byte aligned address
// - x0 = DTB physical address, x1-x3 = 0

static constexpr rex::hal::GPA RAM_BASE          = 0x0;       // GPA 0
static constexpr rex::hal::GPA KERNEL_LOAD_ADDR  = 0x80000;   // RAM + 512KB
static constexpr rex::hal::GPA DTB_LOAD_ADDR     = 0x4000000; // RAM + 64MB
static constexpr rex::hal::GPA INITRD_LOAD_ADDR  = 0x8000000; // RAM + 128MB

// ARM64 Image header magic
static constexpr uint32_t ARM64_MAGIC = 0x644D5241; // "ARM\x64"
static constexpr size_t   ARM64_MAGIC_OFFSET = 56;

// Flattened Device Tree constants
static constexpr uint32_t FDT_MAGIC              = 0xD00DFEED;
static constexpr uint32_t FDT_VERSION            = 17;
static constexpr uint32_t FDT_LAST_COMP_VERSION  = 16;
static constexpr uint32_t FDT_BEGIN_NODE         = 0x00000001;
static constexpr uint32_t FDT_END_NODE           = 0x00000002;
static constexpr uint32_t FDT_PROP               = 0x00000003;
static constexpr uint32_t FDT_END                = 0x00000009;

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

template <typename T>
T host_to_be(T value) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    }
    return value;
}

void append_bytes(std::vector<uint8_t>& out, const void* data, size_t size) {
    const auto* begin = static_cast<const uint8_t*>(data);
    out.insert(out.end(), begin, begin + size);
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    const auto be = host_to_be(value);
    append_bytes(out, &be, sizeof(be));
}

void append_be64(std::vector<uint8_t>& out, uint64_t value) {
    const auto be = host_to_be(value);
    append_bytes(out, &be, sizeof(be));
}

void pad_to_4(std::vector<uint8_t>& out) {
    while ((out.size() & 3U) != 0) {
        out.push_back(0);
    }
}

class FdtBuilder {
public:
    void begin_node(std::string_view name) {
        append_be32(structure_, FDT_BEGIN_NODE);
        structure_.insert(structure_.end(), name.begin(), name.end());
        structure_.push_back('\0');
        pad_to_4(structure_);
    }

    void end_node() {
        append_be32(structure_, FDT_END_NODE);
    }

    void property_string(std::string_view name, std::string_view value) {
        std::vector<uint8_t> data(value.begin(), value.end());
        data.push_back('\0');
        property(name, data);
    }

    void property_u32(std::string_view name, uint32_t value) {
        std::vector<uint8_t> data;
        append_be32(data, value);
        property(name, data);
    }

    void property_cells(std::string_view name, const std::vector<uint32_t>& cells) {
        std::vector<uint8_t> data;
        data.reserve(cells.size() * sizeof(uint32_t));
        for (uint32_t cell : cells) {
            append_be32(data, cell);
        }
        property(name, data);
    }

    std::vector<uint8_t> build() {
        append_be32(structure_, FDT_END);

        std::vector<uint8_t> out;
        const uint32_t off_mem_rsvmap = 40;
        const uint32_t off_dt_struct =
            off_mem_rsvmap + static_cast<uint32_t>(2 * sizeof(uint64_t));
        const uint32_t off_dt_strings =
            off_dt_struct + static_cast<uint32_t>(structure_.size());
        const uint32_t totalsize =
            off_dt_strings + static_cast<uint32_t>(strings_.size());

        out.reserve(totalsize);
        append_be32(out, FDT_MAGIC);
        append_be32(out, totalsize);
        append_be32(out, off_dt_struct);
        append_be32(out, off_dt_strings);
        append_be32(out, off_mem_rsvmap);
        append_be32(out, FDT_VERSION);
        append_be32(out, FDT_LAST_COMP_VERSION);
        append_be32(out, 0); // boot_cpuid_phys
        append_be32(out, static_cast<uint32_t>(strings_.size()));
        append_be32(out, static_cast<uint32_t>(structure_.size()));

        append_be64(out, 0);
        append_be64(out, 0);

        out.insert(out.end(), structure_.begin(), structure_.end());
        out.insert(out.end(), strings_.begin(), strings_.end());
        return out;
    }

private:
    void property(std::string_view name, const std::vector<uint8_t>& data) {
        append_be32(structure_, FDT_PROP);
        append_be32(structure_, static_cast<uint32_t>(data.size()));
        append_be32(structure_, string_offset(name));
        structure_.insert(structure_.end(), data.begin(), data.end());
        pad_to_4(structure_);
    }

    uint32_t string_offset(std::string_view name) {
        const std::string key(name);
        if (const auto it = strings_index_.find(key); it != strings_index_.end()) {
            return it->second;
        }

        const uint32_t offset = static_cast<uint32_t>(strings_.size());
        strings_.insert(strings_.end(), key.begin(), key.end());
        strings_.push_back('\0');
        strings_index_.emplace(key, offset);
        return offset;
    }

    std::vector<uint8_t> structure_;
    std::vector<uint8_t> strings_;
    std::unordered_map<std::string, uint32_t> strings_index_;
};

std::vector<uint32_t> cells_for_u64(uint64_t value) {
    return {
        static_cast<uint32_t>(value >> 32),
        static_cast<uint32_t>(value & 0xFFFF'FFFFULL),
    };
}

/// Generate a minimal device tree blob for RexPlayer ARM64 guest
///
/// This creates a FDT with:
/// - /chosen: bootargs, initrd-start/end
/// - /memory: ram base and size
/// - /cpus: one cpu node
static std::vector<uint8_t> generate_minimal_dtb(
    rex::hal::GPA ram_base,
    rex::hal::MemSize ram_size,
    const std::string& cmdline,
    rex::hal::GPA initrd_start,
    rex::hal::GPA initrd_end)
{
    FdtBuilder builder;

    builder.begin_node("");
    builder.property_string("compatible", "linux,dummy-virt");
    builder.property_string("model", "RexPlayer ARM64");
    builder.property_u32("#address-cells", 2);
    builder.property_u32("#size-cells", 2);

    builder.begin_node("chosen");
    if (!cmdline.empty()) {
        builder.property_string("bootargs", cmdline);
    }
    if (initrd_start != 0 && initrd_end > initrd_start) {
        builder.property_cells("linux,initrd-start", cells_for_u64(initrd_start));
        builder.property_cells("linux,initrd-end", cells_for_u64(initrd_end));
    }
    builder.end_node();

    builder.begin_node("memory@0");
    builder.property_string("device_type", "memory");
    auto reg_cells = cells_for_u64(ram_base);
    const auto size_cells = cells_for_u64(ram_size);
    reg_cells.insert(reg_cells.end(), size_cells.begin(), size_cells.end());
    builder.property_cells("reg", reg_cells);
    builder.end_node();

    builder.begin_node("cpus");
    builder.property_u32("#address-cells", 1);
    builder.property_u32("#size-cells", 0);
    builder.begin_node("cpu@0");
    builder.property_string("device_type", "cpu");
    builder.property_string("compatible", "arm,armv8");
    builder.property_u32("reg", 0);
    builder.end_node();
    builder.end_node();

    // PL011 UART at 0x09000000 — console output
    builder.begin_node("pl011@9000000");
    builder.property_string("compatible", "arm,pl011\0arm,primecell");
    auto uart_reg = cells_for_u64(0x09000000);
    auto uart_size = cells_for_u64(0x1000);
    uart_reg.insert(uart_reg.end(), uart_size.begin(), uart_size.end());
    builder.property_cells("reg", uart_reg);
    builder.end_node();

    // stdout-path for kernel console
    // (reopen chosen to add stdout-path — or we set it in cmdline)

    builder.end_node();
    return builder.build();
}

} // namespace

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

    // 2. Check ARM64 Image magic (optional — allow raw binaries)
    bool is_linux_image = false;
    if (kernel.size() >= ARM64_MAGIC_OFFSET + 4) {
        uint32_t magic = 0;
        memcpy(&magic, &kernel[ARM64_MAGIC_OFFSET], 4);
        is_linux_image = (magic == ARM64_MAGIC);
    }
    // Raw binaries (no magic) are loaded directly at KERNEL_LOAD_ADDR
    (void)is_linux_image;

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

    // 5. Generate a minimal but valid DTB for direct kernel boot.
    auto dtb = generate_minimal_dtb(RAM_BASE, mem.total_allocated(),
                                    params.cmdline, initrd_start, initrd_end);
    if (dtb.empty()) {
        return std::unexpected(rex::hal::HalError::InternalError);
    }

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
    // Set ARM64 registers via the IVcpu adapter:
    //   rax (X0) = DTB address
    //   rip (PC) = kernel entry point
    //   rflags (CPSR) = EL1h mode (0x3C5)
    rex::hal::X86Regs regs{};
    regs.rax = DTB_LOAD_ADDR;  // X0 = DTB address
    regs.rbx = 0;              // X1 = 0
    regs.rcx = 0;              // X2 = 0
    regs.rdx = 0;              // X3 = 0
    regs.rip = KERNEL_LOAD_ADDR;  // PC = kernel entry
    regs.rflags = 0x3C5;       // CPSR = EL1h, DAIF masked

    auto regs_result = vcpu.set_regs(regs);
    if (!regs_result) return regs_result;

    return {};
}

} // namespace rex::vmm
