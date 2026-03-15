#include <gtest/gtest.h>
#include "rex/hal/types.h"

using namespace rex::hal;

TEST(HalTypes, VcpuExitReasonValues) {
    VcpuExit exit{};
    exit.reason = VcpuExit::Reason::Hlt;
    EXPECT_EQ(exit.reason, VcpuExit::Reason::Hlt);
}

TEST(HalTypes, IoAccessStruct) {
    IoAccess io{};
    io.port = 0x3F8;
    io.size = 1;
    io.is_write = true;
    io.data = 'A';

    EXPECT_EQ(io.port, 0x3F8);
    EXPECT_EQ(io.size, 1);
    EXPECT_TRUE(io.is_write);
    EXPECT_EQ(io.data, static_cast<uint32_t>('A'));
}

TEST(HalTypes, MmioAccessStruct) {
    MmioAccess mmio{};
    mmio.address = 0xFED00000;
    mmio.size = 4;
    mmio.is_write = false;
    mmio.data = 0;

    EXPECT_EQ(mmio.address, 0xFED00000ULL);
    EXPECT_EQ(mmio.size, 4);
    EXPECT_FALSE(mmio.is_write);
}

TEST(HalTypes, MemoryRegionStruct) {
    MemoryRegion region{};
    region.slot = 0;
    region.guest_phys_addr = 0;
    region.size = 512ULL * 1024 * 1024;
    region.userspace_addr = 0x7F0000000000;
    region.readonly = false;

    EXPECT_EQ(region.size, 512ULL * 1024 * 1024);
    EXPECT_FALSE(region.readonly);
}

TEST(HalTypes, HalErrorToString) {
    EXPECT_STREQ(hal_error_str(HalError::Ok), "Ok");
    EXPECT_STREQ(hal_error_str(HalError::NotSupported), "NotSupported");
    EXPECT_STREQ(hal_error_str(HalError::VcpuRunFailed), "VcpuRunFailed");
    EXPECT_STREQ(hal_error_str(HalError::MemoryMappingFailed), "MemoryMappingFailed");
}

TEST(HalTypes, HalResultSuccess) {
    HalResult<int> result = 42;
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(HalTypes, HalResultError) {
    HalResult<int> result = std::unexpected(HalError::NotSupported);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), HalError::NotSupported);
}

TEST(HalTypes, X86RegsInit) {
    X86Regs regs{};
    regs.rip = 0x100000;
    regs.rsp = 0x7000;
    regs.rflags = 0x2;

    EXPECT_EQ(regs.rip, 0x100000ULL);
    EXPECT_EQ(regs.rsp, 0x7000ULL);
    EXPECT_EQ(regs.rflags, 0x2ULL);
    EXPECT_EQ(regs.rax, 0ULL);
}
