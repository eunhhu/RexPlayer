#include "rex/vmm/memory_manager.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <unistd.h>
#endif
#endif

#include <cstdlib>
#include <cstring>

namespace rex::vmm {

void MemoryManager::release_allocation(const Allocation& alloc) {
    if (!alloc.host_ptr) {
        return;
    }

    switch (alloc.backend) {
        case MemoryManager::AllocationBackend::VirtualAlloc:
#ifdef _WIN32
            VirtualFree(alloc.host_ptr, 0, MEM_RELEASE);
#endif
            break;
        case MemoryManager::AllocationBackend::Mmap:
#if !defined(_WIN32)
            munmap(alloc.host_ptr, alloc.size);
#endif
            break;
        case MemoryManager::AllocationBackend::PosixMemalign:
            free(alloc.host_ptr);
            break;
    }
}

MemoryManager::MemoryManager(rex::hal::IMemoryManager& hal_mem)
    : hal_mem_(hal_mem) {}

MemoryManager::~MemoryManager() {
    for (auto& alloc : allocations_) {
        MemoryManager::release_allocation(alloc);
    }
}

rex::hal::HalResult<void> MemoryManager::add_ram(rex::hal::GPA gpa, rex::hal::MemSize size) {
    // Allocate host memory
    void* host_ptr = nullptr;
    AllocationBackend backend = AllocationBackend::Mmap;

#ifdef _WIN32
    host_ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!host_ptr) {
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }
    backend = AllocationBackend::VirtualAlloc;
#elif defined(__APPLE__) && defined(__aarch64__)
    const long page_size = sysconf(_SC_PAGESIZE);
    const size_t alignment = page_size > 0 ? static_cast<size_t>(page_size) : 16384;
    if (posix_memalign(&host_ptr, alignment, static_cast<size_t>(size)) != 0) {
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }
    backend = AllocationBackend::PosixMemalign;
#else
    host_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (host_ptr == MAP_FAILED) {
        return std::unexpected(rex::hal::HalError::OutOfMemory);
    }

    // Advise the kernel we want huge pages if possible
#ifdef MADV_HUGEPAGE
    madvise(host_ptr, size, MADV_HUGEPAGE);
#endif
    backend = AllocationBackend::Mmap;
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
        MemoryManager::release_allocation({slot, gpa, size, host_ptr, backend});
        return result;
    }

    allocations_.push_back({slot, gpa, size, host_ptr, backend});
    total_allocated_ += size;
    return {};
}

const MemoryManager::Allocation* MemoryManager::find_allocation(rex::hal::GPA gpa) const {
    for (const auto& alloc : allocations_) {
        if (gpa >= alloc.gpa && gpa < alloc.gpa + alloc.size) {
            return &alloc;
        }
    }
    return nullptr;
}

rex::hal::HalResult<void*> MemoryManager::get_host_ptr(rex::hal::GPA gpa) const {
    if (const auto* alloc = find_allocation(gpa)) {
        auto offset = gpa - alloc->gpa;
        return static_cast<uint8_t*>(alloc->host_ptr) + offset;
    }
    return std::unexpected(rex::hal::HalError::InvalidParameter);
}

rex::hal::HalResult<void> MemoryManager::write(rex::hal::GPA gpa, const void* data, size_t len) {
    const auto* alloc = find_allocation(gpa);
    if (!alloc) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    auto offset = static_cast<size_t>(gpa - alloc->gpa);
    if (offset > alloc->size || len > alloc->size - offset) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    auto* dst = static_cast<uint8_t*>(alloc->host_ptr) + offset;
    memcpy(dst, data, len);
    return {};
}

rex::hal::HalResult<void> MemoryManager::read(rex::hal::GPA gpa, void* buf, size_t len) const {
    const auto* alloc = find_allocation(gpa);
    if (!alloc) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    auto offset = static_cast<size_t>(gpa - alloc->gpa);
    if (offset > alloc->size || len > alloc->size - offset) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    const auto* src = static_cast<const uint8_t*>(alloc->host_ptr) + offset;
    memcpy(buf, src, len);
    return {};
}

} // namespace rex::vmm
