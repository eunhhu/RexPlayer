#include "rex/vmm/memory_manager.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <cstring>

namespace rex::vmm {

MemoryManager::MemoryManager(rex::hal::IMemoryManager& hal_mem)
    : hal_mem_(hal_mem) {}

MemoryManager::~MemoryManager() {
    for (auto& alloc : allocations_) {
        if (alloc.host_ptr) {
#ifdef _WIN32
            VirtualFree(alloc.host_ptr, 0, MEM_RELEASE);
#else
            munmap(alloc.host_ptr, alloc.size);
#endif
        }
    }
}

rex::hal::HalResult<void> MemoryManager::add_ram(rex::hal::GPA gpa, rex::hal::MemSize size) {
    // Allocate host memory
    void* host_ptr = nullptr;

#ifdef _WIN32
    host_ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!host_ptr) {
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }
#else
    host_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (host_ptr == MAP_FAILED) {
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }

    // Advise the kernel we want huge pages if possible
    madvise(host_ptr, size, MADV_HUGEPAGE);
#endif

    // Zero the memory
    memset(host_ptr, 0, size);

    // Map into guest via HAL
    uint32_t slot = next_slot_++;
    rex::hal::MemoryRegion region{};
    region.slot = slot;
    region.guest_phys_addr = gpa;
    region.size = size;
    region.userspace_addr = reinterpret_cast<rex::hal::HVA>(host_ptr);
    region.readonly = false;

    auto result = hal_mem_.map_region(region);
    if (!result) {
#ifdef _WIN32
        VirtualFree(host_ptr, 0, MEM_RELEASE);
#else
        munmap(host_ptr, size);
#endif
        return result;
    }

    allocations_.push_back({slot, gpa, size, host_ptr});
    total_allocated_ += size;
    return {};
}

rex::hal::HalResult<void*> MemoryManager::get_host_ptr(rex::hal::GPA gpa) const {
    for (const auto& alloc : allocations_) {
        if (gpa >= alloc.gpa && gpa < alloc.gpa + alloc.size) {
            auto offset = gpa - alloc.gpa;
            return static_cast<uint8_t*>(alloc.host_ptr) + offset;
        }
    }
    return std::unexpected(rex::hal::HalError::InvalidParameter);
}

rex::hal::HalResult<void> MemoryManager::write(rex::hal::GPA gpa, const void* data, size_t len) {
    auto ptr_result = get_host_ptr(gpa);
    if (!ptr_result) return std::unexpected(ptr_result.error());

    memcpy(*ptr_result, data, len);
    return {};
}

rex::hal::HalResult<void> MemoryManager::read(rex::hal::GPA gpa, void* buf, size_t len) const {
    auto ptr_result = get_host_ptr(gpa);
    if (!ptr_result) return std::unexpected(ptr_result.error());

    memcpy(buf, *ptr_result, len);
    return {};
}

} // namespace rex::vmm
