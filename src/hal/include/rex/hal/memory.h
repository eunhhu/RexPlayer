#pragma once

#include "rex/hal/types.h"
#include <vector>

namespace rex::hal {

/// Interface for guest physical memory management
class IMemoryManager {
public:
    virtual ~IMemoryManager() = default;

    /// Map a host memory region into the guest physical address space
    virtual HalResult<void> map_region(const MemoryRegion& region) = 0;

    /// Unmap a previously mapped memory region
    virtual HalResult<void> unmap_region(uint32_t slot) = 0;

    /// Translate a guest physical address to a host virtual address
    virtual HalResult<HVA> gpa_to_hva(GPA gpa) const = 0;

    /// Get all mapped memory regions
    virtual std::vector<MemoryRegion> get_regions() const = 0;
};

} // namespace rex::hal
