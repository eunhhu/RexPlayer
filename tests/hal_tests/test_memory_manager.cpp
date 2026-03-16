#include <gtest/gtest.h>

#include "rex/hal/memory.h"
#include "rex/vmm/memory_manager.h"

#include <algorithm>
#include <vector>

using namespace rex::hal;
using namespace rex::vmm;

namespace {

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

} // namespace

TEST(MemoryManager, WriteAndReadWithinAllocation) {
    MockMemoryManager hal_mem;
    MemoryManager mem(hal_mem);

    ASSERT_TRUE(mem.add_ram(0, 4096).has_value());

    const uint32_t value = 0x12345678;
    ASSERT_TRUE(mem.write(128, &value, sizeof(value)).has_value());

    uint32_t read_back = 0;
    ASSERT_TRUE(mem.read(128, &read_back, sizeof(read_back)).has_value());
    EXPECT_EQ(read_back, value);
}

TEST(MemoryManager, RejectsOutOfBoundsWrite) {
    MockMemoryManager hal_mem;
    MemoryManager mem(hal_mem);

    ASSERT_TRUE(mem.add_ram(0, 4096).has_value());

    const uint32_t value = 0xDEADBEEF;
    auto result = mem.write(4094, &value, sizeof(value));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), HalError::InvalidParameter);
}

TEST(MemoryManager, RejectsOutOfBoundsRead) {
    MockMemoryManager hal_mem;
    MemoryManager mem(hal_mem);

    ASSERT_TRUE(mem.add_ram(0, 4096).has_value());

    uint32_t read_back = 0;
    auto result = mem.read(4094, &read_back, sizeof(read_back));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), HalError::InvalidParameter);
}
