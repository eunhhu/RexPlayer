#include "memory_optimizer.h"
#include "rex/vmm/memory_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef __linux__
#include <fstream>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace rex::vmm {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MemoryOptimizer::MemoryOptimizer(MemoryManager& mem_mgr)
    : mem_mgr_(mem_mgr) {}

MemoryOptimizer::~MemoryOptimizer() = default;

// ---------------------------------------------------------------------------
// On-demand paging / page tracking
// ---------------------------------------------------------------------------

void MemoryOptimizer::init_page_tracking(rex::hal::GPA base, rex::hal::MemSize size) {
    std::lock_guard<std::mutex> lock(tracking_mutex_);

    tracking_base_ = base;
    tracking_size_ = size;

    size_t num_pages = static_cast<size_t>((size + PAGE_SIZE - 1) / PAGE_SIZE);
    accessed_bitmap_.assign(num_pages, false);
    dirty_bitmap_.assign(num_pages, false);
}

void MemoryOptimizer::mark_accessed(rex::hal::GPA gpa) {
    if (!is_tracked(gpa)) return;

    std::lock_guard<std::mutex> lock(tracking_mutex_);
    size_t idx = gpa_to_page_index(gpa);
    if (idx < accessed_bitmap_.size()) {
        accessed_bitmap_[idx] = true;
    }
}

void MemoryOptimizer::mark_dirty(rex::hal::GPA gpa) {
    if (!is_tracked(gpa)) return;

    std::lock_guard<std::mutex> lock(tracking_mutex_);
    size_t idx = gpa_to_page_index(gpa);
    if (idx < dirty_bitmap_.size()) {
        dirty_bitmap_[idx] = true;
        accessed_bitmap_[idx] = true; // dirty implies accessed
    }
}

bool MemoryOptimizer::is_accessed(rex::hal::GPA gpa) const {
    if (!is_tracked(gpa)) return false;

    std::lock_guard<std::mutex> lock(tracking_mutex_);
    size_t idx = gpa_to_page_index(gpa);
    if (idx < accessed_bitmap_.size()) {
        return accessed_bitmap_[idx];
    }
    return false;
}

bool MemoryOptimizer::is_dirty(rex::hal::GPA gpa) const {
    if (!is_tracked(gpa)) return false;

    std::lock_guard<std::mutex> lock(tracking_mutex_);
    size_t idx = gpa_to_page_index(gpa);
    if (idx < dirty_bitmap_.size()) {
        return dirty_bitmap_[idx];
    }
    return false;
}

std::vector<rex::hal::GPA> MemoryOptimizer::get_dirty_pages() const {
    std::lock_guard<std::mutex> lock(tracking_mutex_);

    std::vector<rex::hal::GPA> result;
    result.reserve(dirty_bitmap_.size() / 8); // heuristic: ~12.5% dirty is common

    for (size_t i = 0; i < dirty_bitmap_.size(); ++i) {
        if (dirty_bitmap_[i]) {
            result.push_back(tracking_base_ + i * PAGE_SIZE);
        }
    }
    return result;
}

void MemoryOptimizer::reset_dirty_tracking() {
    std::lock_guard<std::mutex> lock(tracking_mutex_);
    std::fill(dirty_bitmap_.begin(), dirty_bitmap_.end(), false);
}

size_t MemoryOptimizer::tracked_page_count() const {
    std::lock_guard<std::mutex> lock(tracking_mutex_);
    return accessed_bitmap_.size();
}

// ---------------------------------------------------------------------------
// Huge page support
// ---------------------------------------------------------------------------

bool MemoryOptimizer::detect_huge_page_support() {
#ifdef __linux__
    // Check if THP is available via /sys/kernel/mm/transparent_hugepage/enabled
    std::ifstream thp_file("/sys/kernel/mm/transparent_hugepage/enabled");
    if (thp_file.is_open()) {
        std::string line;
        std::getline(thp_file, line);
        // If the file exists and contains [always] or [madvise], THP is available
        if (line.find("[always]") != std::string::npos ||
            line.find("[madvise]") != std::string::npos) {
            return true;
        }
    }

    // Also check for static huge pages
    std::ifstream nr_file("/proc/sys/vm/nr_hugepages");
    if (nr_file.is_open()) {
        int nr = 0;
        nr_file >> nr;
        if (nr > 0) return true;
    }

    return false;

#elif defined(__APPLE__)
    // macOS supports "superpages" via VM_FLAGS_SUPERPAGE_SIZE_2MB in mmap
    // but there's no runtime query; the capability is always present on x86_64/arm64
    return true;

#elif defined(_WIN32)
    // Windows supports large pages if the process has SeLockMemoryPrivilege
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    bool supported = LookupPrivilegeValue(nullptr, SE_LOCK_MEMORY_NAME, &luid) != 0;
    CloseHandle(token);
    return supported;

#else
    return false;
#endif
}

void MemoryOptimizer::configure_huge_pages(const HugePageConfig& config) {
    huge_page_config_ = config;
}

void MemoryOptimizer::promote_to_huge_pages(rex::hal::GPA base, rex::hal::MemSize size) {
    if (!huge_page_config_.enabled) return;

    auto host_ptr_result = mem_mgr_.get_host_ptr(base);
    if (!host_ptr_result) return;

    void* ptr = *host_ptr_result;

#ifdef __linux__
    if (huge_page_config_.transparent) {
        // Advise the kernel to use THP for this region
        madvise(ptr, size, MADV_HUGEPAGE);
    }
#elif defined(__APPLE__)
    // macOS: madvise is limited; superpage promotion happens at mmap time.
    // We can still hint about access patterns.
    madvise(ptr, size, MADV_SEQUENTIAL);
    (void)ptr;
    (void)size;
#elif defined(_WIN32)
    // Windows: large pages must be allocated at VirtualAlloc time,
    // not promoted after the fact. This is a no-op for existing allocations.
    (void)ptr;
    (void)size;
#else
    (void)ptr;
    (void)size;
#endif
}

uint64_t MemoryOptimizer::count_huge_pages(rex::hal::GPA base, rex::hal::MemSize size) const {
#ifdef __linux__
    // On Linux we can check /proc/self/smaps for AnonHugePages
    auto host_ptr_result = mem_mgr_.get_host_ptr(base);
    if (!host_ptr_result) return 0;

    void* ptr = *host_ptr_result;
    uint64_t count = 0;

    // Walk the region in huge-page-sized steps and check mincore
    // A simpler approach: read /proc/self/smaps for the mapping
    // For now, estimate based on alignment
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    auto aligned_start = (addr + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
    auto end = addr + size;

    if (aligned_start < end) {
        count = (end - aligned_start) / HUGE_PAGE_SIZE;
    }
    return count;

#else
    (void)base;
    (void)size;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Memory statistics
// ---------------------------------------------------------------------------

MemoryStats MemoryOptimizer::compute_stats(rex::hal::GPA base, rex::hal::MemSize size) const {
    MemoryStats stats{};
    stats.total_bytes = size;

    auto host_ptr_result = mem_mgr_.get_host_ptr(base);
    if (!host_ptr_result) {
        return stats;
    }

    const auto* mem = static_cast<const uint8_t*>(*host_ptr_result);
    uint64_t num_pages = size / PAGE_SIZE;
    uint64_t used_pages = 0;
    uint64_t zero_pages = 0;

    // Scan pages to determine usage
    for (uint64_t i = 0; i < num_pages; ++i) {
        const void* page_ptr = mem + i * PAGE_SIZE;
        if (is_zero_page(page_ptr)) {
            ++zero_pages;
        } else {
            ++used_pages;
        }
    }

    stats.used_bytes = used_pages * PAGE_SIZE;
    stats.zero_pages = zero_pages;
    stats.balloon_bytes = balloon_bytes_.load(std::memory_order_relaxed);
    stats.shared_bytes = estimate_ksm_savings();
    stats.huge_pages = count_huge_pages(base, size);

    // Count dirty pages from our tracking
    {
        std::lock_guard<std::mutex> lock(tracking_mutex_);
        uint64_t dirty = 0;
        for (size_t j = 0; j < dirty_bitmap_.size(); ++j) {
            if (dirty_bitmap_[j]) ++dirty;
        }
        stats.dirty_pages = dirty;
    }

    return stats;
}

// ---------------------------------------------------------------------------
// Balloon integration
// ---------------------------------------------------------------------------

void MemoryOptimizer::balloon_inflate(uint64_t bytes) {
    balloon_bytes_.fetch_add(bytes, std::memory_order_relaxed);
}

void MemoryOptimizer::balloon_deflate(uint64_t bytes) {
    uint64_t current = balloon_bytes_.load(std::memory_order_relaxed);
    uint64_t to_subtract = (std::min)(current, bytes);
    balloon_bytes_.fetch_sub(to_subtract, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// KSM (Kernel Same-page Merging)
// ---------------------------------------------------------------------------

void MemoryOptimizer::configure_ksm(const KsmConfig& config) {
    ksm_config_ = config;

#ifdef __linux__
    if (config.enabled) {
        // Write KSM configuration to sysfs (requires root privileges)
        {
            std::ofstream run_file("/sys/kernel/mm/ksm/run");
            if (run_file.is_open()) {
                run_file << "1";
            }
        }
        {
            std::ofstream scan_file("/sys/kernel/mm/ksm/pages_to_scan");
            if (scan_file.is_open()) {
                scan_file << config.pages_to_scan;
            }
        }
        {
            std::ofstream sleep_file("/sys/kernel/mm/ksm/sleep_millisecs");
            if (sleep_file.is_open()) {
                sleep_file << config.sleep_ms;
            }
        }
    } else {
        std::ofstream run_file("/sys/kernel/mm/ksm/run");
        if (run_file.is_open()) {
            run_file << "0";
        }
    }
#endif
}

void MemoryOptimizer::enable_ksm_on_region(rex::hal::GPA base, rex::hal::MemSize size) {
    auto host_ptr_result = mem_mgr_.get_host_ptr(base);
    if (!host_ptr_result) return;

    void* ptr = *host_ptr_result;

#ifdef __linux__
    // MADV_MERGEABLE tells KSM to scan this region for identical pages
    madvise(ptr, size, MADV_MERGEABLE);
#else
    // KSM is Linux-specific; no-op on other platforms
    (void)ptr;
    (void)size;
#endif
}

void MemoryOptimizer::disable_ksm_on_region(rex::hal::GPA base, rex::hal::MemSize size) {
    auto host_ptr_result = mem_mgr_.get_host_ptr(base);
    if (!host_ptr_result) return;

    void* ptr = *host_ptr_result;

#ifdef __linux__
    madvise(ptr, size, MADV_UNMERGEABLE);
#else
    (void)ptr;
    (void)size;
#endif
}

uint64_t MemoryOptimizer::estimate_ksm_savings() const {
#ifdef __linux__
    // Read pages_sharing from KSM sysfs
    std::ifstream sharing_file("/sys/kernel/mm/ksm/pages_sharing");
    if (sharing_file.is_open()) {
        uint64_t pages_sharing = 0;
        sharing_file >> pages_sharing;
        return pages_sharing * PAGE_SIZE;
    }
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

size_t MemoryOptimizer::gpa_to_page_index(rex::hal::GPA gpa) const {
    return static_cast<size_t>((gpa - tracking_base_) / PAGE_SIZE);
}

bool MemoryOptimizer::is_tracked(rex::hal::GPA gpa) const {
    return gpa >= tracking_base_ && gpa < tracking_base_ + tracking_size_;
}

bool MemoryOptimizer::is_zero_page(const void* page_ptr) const {
    // Check 8 bytes at a time for efficiency
    const auto* qwords = static_cast<const uint64_t*>(page_ptr);
    constexpr size_t qwords_per_page = PAGE_SIZE / sizeof(uint64_t);

    for (size_t i = 0; i < qwords_per_page; ++i) {
        if (qwords[i] != 0) return false;
    }
    return true;
}

} // namespace rex::vmm
