#include <gtest/gtest.h>

#include "rex/hal/memory.h"
#include "rex/hal/vcpu.h"
#include "rex/vmm/boot.h"
#include "rex/vmm/memory_manager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rex::hal;
using namespace rex::vmm;

namespace {

constexpr GPA kKernelLoadAddr = 0x80000;
constexpr GPA kDtbLoadAddr = 0x4000000;
constexpr size_t kArm64MagicOffset = 56;
constexpr uint32_t kArm64Magic = 0x644D5241;

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24)
         | (static_cast<uint32_t>(data[1]) << 16)
         | (static_cast<uint32_t>(data[2]) << 8)
         | static_cast<uint32_t>(data[3]);
}

class MockMemoryManager : public IMemoryManager {
public:
    HalResult<void> map_region(const MemoryRegion& region) override {
        regions_.push_back(region);
        return {};
    }

    HalResult<void> unmap_region(uint32_t slot) override {
        auto it = std::find_if(regions_.begin(), regions_.end(),
            [slot](const MemoryRegion& region) { return region.slot == slot; });
        if (it == regions_.end()) {
            return std::unexpected(HalError::InvalidParameter);
        }
        regions_.erase(it);
        return {};
    }

    HalResult<HVA> gpa_to_hva(GPA gpa) const override {
        for (const auto& region : regions_) {
            if (gpa >= region.guest_phys_addr && gpa < region.guest_phys_addr + region.size) {
                return region.userspace_addr + (gpa - region.guest_phys_addr);
            }
        }
        return std::unexpected(HalError::InvalidParameter);
    }

    std::vector<MemoryRegion> get_regions() const override {
        return regions_;
    }

private:
    std::vector<MemoryRegion> regions_;
};

class MockVcpu : public IVcpu {
public:
    HalResult<VcpuExit> run() override {
        return std::unexpected(HalError::NotSupported);
    }

    VcpuId id() const override {
        return 0;
    }

    HalResult<X86Regs> get_regs() const override {
        return regs_;
    }

    HalResult<void> set_regs(const X86Regs& regs) override {
        regs_ = regs;
        return {};
    }

    HalResult<X86Sregs> get_sregs() const override {
        return sregs_;
    }

    HalResult<void> set_sregs(const X86Sregs& sregs) override {
        sregs_ = sregs;
        return {};
    }

    HalResult<void> inject_interrupt(uint32_t) override {
        return {};
    }

    HalResult<uint64_t> get_msr(uint32_t) const override {
        return std::unexpected(HalError::NotSupported);
    }

    HalResult<void> set_msr(uint32_t, uint64_t) override {
        return std::unexpected(HalError::NotSupported);
    }

private:
    X86Regs regs_{};
    X86Sregs sregs_{};
};

} // namespace

TEST(Arm64Boot, GeneratesValidDtbAndBootRegisters) {
    MockMemoryManager hal_mem;
    MemoryManager mem(hal_mem);
    MockVcpu vcpu;

    ASSERT_TRUE(mem.add_ram(kKernelLoadAddr, 2 * 1024 * 1024).has_value());
    ASSERT_TRUE(mem.add_ram(kDtbLoadAddr, 64 * 1024).has_value());

    const auto kernel_path =
        std::filesystem::temp_directory_path() / "rexplayer-arm64-kernel-test.bin";

    std::vector<uint8_t> kernel(kArm64MagicOffset + sizeof(uint32_t), 0);
    std::memcpy(kernel.data() + kArm64MagicOffset, &kArm64Magic, sizeof(kArm64Magic));
    {
        std::ofstream file(kernel_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file.write(reinterpret_cast<const char*>(kernel.data()),
                   static_cast<std::streamsize>(kernel.size()));
        ASSERT_TRUE(file.good());
    }

    BootParams params;
    params.kernel_path = kernel_path.string();
    params.cmdline = "console=ttyAMA0 root=/dev/ram0";

    const auto cleanup = [&] { std::filesystem::remove(kernel_path); };

    auto boot_result = setup_direct_boot_arm64(vcpu, mem, params);
    cleanup();
    ASSERT_TRUE(boot_result.has_value()) << hal_error_str(boot_result.error());

    auto regs_result = vcpu.get_regs();
    ASSERT_TRUE(regs_result.has_value());
    EXPECT_EQ(regs_result->rax, kDtbLoadAddr);
    EXPECT_EQ(regs_result->rbx, 0U);
    EXPECT_EQ(regs_result->rcx, 0U);
    EXPECT_EQ(regs_result->rdx, 0U);
    EXPECT_EQ(regs_result->rip, kKernelLoadAddr);
    EXPECT_EQ(regs_result->rflags, 0x3C5U);

    std::array<uint8_t, 8> header{};
    ASSERT_TRUE(mem.read(kDtbLoadAddr, header.data(), header.size()).has_value());
    EXPECT_EQ(read_be32(header.data()), 0xD00DFEEDU);

    const uint32_t total_size = read_be32(header.data() + 4);
    ASSERT_GT(total_size, 40U);

    std::vector<uint8_t> dtb(total_size);
    ASSERT_TRUE(mem.read(kDtbLoadAddr, dtb.data(), dtb.size()).has_value());

    const std::string dtb_text(dtb.begin(), dtb.end());
    EXPECT_NE(dtb_text.find("console=ttyAMA0"), std::string::npos);
    EXPECT_NE(dtb_text.find("linux,dummy-virt"), std::string::npos);
}
