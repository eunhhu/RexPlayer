#pragma once

#include "rex/hal/hypervisor.h"
#include <cstdint>
#include <vector>

namespace rex::vmm {

/// High-level memory manager for the VM
///
/// Allocates host memory (via mmap/VirtualAlloc) and maps it into the guest
/// physical address space through the HAL's IMemoryManager.
class MemoryManager {
public:
    explicit MemoryManager(rex::hal::IMemoryManager& hal_mem);
    ~MemoryManager();

    // Non-copyable, non-movable (owns mmap'd regions)
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    /// Allocate and map guest RAM at a given GPA
    rex::hal::HalResult<void> add_ram(rex::hal::GPA gpa, rex::hal::MemSize size);

    /// Get the host pointer for a guest physical address
    rex::hal::HalResult<void*> get_host_ptr(rex::hal::GPA gpa) const;

    /// Write data to guest physical memory
    rex::hal::HalResult<void> write(rex::hal::GPA gpa, const void* data, size_t len);

    /// Read data from guest physical memory
    rex::hal::HalResult<void> read(rex::hal::GPA gpa, void* buf, size_t len) const;

    /// Get total allocated memory size
    rex::hal::MemSize total_allocated() const { return total_allocated_; }

private:
    struct Allocation {
        uint32_t slot;
        rex::hal::GPA gpa;
        rex::hal::MemSize size;
        void* host_ptr;
    };

    rex::hal::IMemoryManager& hal_mem_;
    std::vector<Allocation> allocations_;
    uint32_t next_slot_ = 0;
    rex::hal::MemSize total_allocated_ = 0;
};

} // namespace rex::vmm
