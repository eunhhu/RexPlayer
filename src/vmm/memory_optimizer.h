#pragma once

#include "rex/hal/types.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace rex::vmm {

class MemoryManager;

/// Memory usage statistics
struct MemoryStats {
    uint64_t total_bytes = 0;      ///< Total guest RAM allocated
    uint64_t used_bytes = 0;       ///< Bytes actually touched (non-zero pages)
    uint64_t balloon_bytes = 0;    ///< Bytes reclaimed by balloon device
    uint64_t shared_bytes = 0;     ///< Bytes deduplicated by KSM
    uint64_t dirty_pages = 0;      ///< Pages modified since last tracking reset
    uint64_t huge_pages = 0;       ///< Number of huge pages (2 MB) in use
    uint64_t zero_pages = 0;       ///< Pages that are entirely zero
};

/// Configuration for Kernel Same-page Merging
struct KsmConfig {
    bool enabled = false;          ///< Whether KSM is active
    uint32_t pages_to_scan = 100;  ///< Pages scanned per KSM sleep cycle
    uint32_t sleep_ms = 20;        ///< Milliseconds between scan cycles
};

/// Configuration for huge page support
struct HugePageConfig {
    bool enabled = false;          ///< Try to use huge pages
    bool transparent = true;       ///< Use THP (Transparent Huge Pages)
    uint64_t huge_page_size = 2ULL * 1024 * 1024; ///< 2 MB default
};

/// Page-level tracking entry
struct PageInfo {
    uint64_t gpa;                  ///< Guest physical address of the page
    bool accessed;                 ///< Page has been read
    bool dirty;                    ///< Page has been written
};

/// Memory optimization manager
///
/// Provides page-level tracking, huge page support detection,
/// memory statistics, dirty page tracking for incremental snapshots,
/// balloon integration, and KSM configuration for multi-instance scenarios.
class MemoryOptimizer {
public:
    /// Standard page size (4 KB)
    static constexpr uint64_t PAGE_SIZE = 4096;

    /// Huge page size (2 MB)
    static constexpr uint64_t HUGE_PAGE_SIZE = 2ULL * 1024 * 1024;

    explicit MemoryOptimizer(MemoryManager& mem_mgr);
    ~MemoryOptimizer();

    // Non-copyable
    MemoryOptimizer(const MemoryOptimizer&) = delete;
    MemoryOptimizer& operator=(const MemoryOptimizer&) = delete;

    // --- On-demand paging ---

    /// Initialize page tracking for the given RAM range
    void init_page_tracking(rex::hal::GPA base, rex::hal::MemSize size);

    /// Mark a page as accessed (called on page fault / EPT violation)
    void mark_accessed(rex::hal::GPA gpa);

    /// Mark a page as dirty (called on write fault)
    void mark_dirty(rex::hal::GPA gpa);

    /// Check if a page has been accessed
    bool is_accessed(rex::hal::GPA gpa) const;

    /// Check if a page has been dirty since last reset
    bool is_dirty(rex::hal::GPA gpa) const;

    /// Get all dirty page GPAs (for incremental snapshots)
    std::vector<rex::hal::GPA> get_dirty_pages() const;

    /// Reset dirty tracking (after snapshot or migration checkpoint)
    void reset_dirty_tracking();

    /// Get the number of tracked pages
    size_t tracked_page_count() const;

    // --- Huge page support ---

    /// Detect and return whether the platform supports huge pages
    static bool detect_huge_page_support();

    /// Configure huge page usage
    void configure_huge_pages(const HugePageConfig& config);

    /// Get current huge page configuration
    const HugePageConfig& huge_page_config() const { return huge_page_config_; }

    /// Attempt to promote a range to huge pages (advisory — OS may ignore)
    void promote_to_huge_pages(rex::hal::GPA base, rex::hal::MemSize size);

    /// Count how many huge pages are currently backing guest memory
    uint64_t count_huge_pages(rex::hal::GPA base, rex::hal::MemSize size) const;

    // --- Memory statistics ---

    /// Compute current memory statistics by scanning guest memory
    MemoryStats compute_stats(rex::hal::GPA base, rex::hal::MemSize size) const;

    // --- Balloon integration ---

    /// Notify that the balloon device has inflated (guest returned pages)
    void balloon_inflate(uint64_t bytes);

    /// Notify that the balloon device has deflated (pages returned to guest)
    void balloon_deflate(uint64_t bytes);

    /// Get current balloon size
    uint64_t balloon_size() const { return balloon_bytes_.load(std::memory_order_relaxed); }

    // --- KSM (Kernel Same-page Merging) ---

    /// Configure KSM for multi-instance memory deduplication
    void configure_ksm(const KsmConfig& config);

    /// Get current KSM configuration
    const KsmConfig& ksm_config() const { return ksm_config_; }

    /// Enable KSM merging on a memory region (calls madvise MADV_MERGEABLE)
    void enable_ksm_on_region(rex::hal::GPA base, rex::hal::MemSize size);

    /// Disable KSM merging on a memory region
    void disable_ksm_on_region(rex::hal::GPA base, rex::hal::MemSize size);

    /// Estimate bytes saved by KSM (reads /sys/kernel/mm/ksm on Linux)
    uint64_t estimate_ksm_savings() const;

private:
    /// Translate a GPA to an index in the page tracking bitmap
    size_t gpa_to_page_index(rex::hal::GPA gpa) const;

    /// Check if a GPA is within the tracked range
    bool is_tracked(rex::hal::GPA gpa) const;

    /// Check if a page is entirely zero
    bool is_zero_page(const void* page_ptr) const;

    MemoryManager& mem_mgr_;

    // Page tracking state
    rex::hal::GPA tracking_base_ = 0;
    rex::hal::MemSize tracking_size_ = 0;
    std::vector<bool> accessed_bitmap_;
    std::vector<bool> dirty_bitmap_;
    mutable std::mutex tracking_mutex_;

    // Balloon state
    std::atomic<uint64_t> balloon_bytes_{0};

    // Configuration
    HugePageConfig huge_page_config_;
    KsmConfig ksm_config_;
};

} // namespace rex::vmm
