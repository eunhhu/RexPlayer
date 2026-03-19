#pragma once

#include "rex/hal/types.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rex::vmm {

/// Represents a single CPU core's properties
struct CoreInfo {
    uint32_t core_id = 0;
    uint32_t thread_id = 0;       // SMT thread index within core
    uint32_t package_id = 0;
    bool is_performance_core = true; // P-core vs E-core on hybrid archs
    uint32_t base_frequency_mhz = 0;
    uint32_t max_frequency_mhz = 0;
};

/// Host CPU topology information
struct CoreTopology {
    uint32_t num_physical_cores = 0;
    uint32_t num_logical_threads = 0;
    uint32_t num_packages = 0;
    bool hybrid = false;          // Intel Alder Lake-style P+E core mix
    uint32_t num_p_cores = 0;
    uint32_t num_e_cores = 0;
    std::vector<CoreInfo> cores;
    std::string vendor;           // "GenuineIntel", "AuthenticAMD", etc.
    std::string model_name;
};

/// CPUID leaf configuration: pass-through, filter, or override
struct CpuidEntry {
    uint32_t function = 0;
    uint32_t index = 0;           // ECX sub-leaf
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    bool passthrough = true;      // true = use host value, false = use overrides
};

/// TSC (Time Stamp Counter) configuration
struct TscConfig {
    uint64_t host_frequency_khz = 0;
    uint64_t guest_frequency_khz = 0;
    bool scaling_enabled = false;
    bool rdtsc_exit_enabled = false; // force VM exit on rdtsc (for debug)
};

/// Thread scheduling priority levels
enum class VcpuPriority : int {
    Idle = -2,
    Low = -1,
    Normal = 0,
    High = 1,
    Realtime = 2,
};

/// Pause-loop exiting (PLE) configuration
struct PleConfig {
    /// Number of PAUSE iterations before triggering a VM exit
    uint32_t ple_gap = 128;
    /// Maximum wait window in cycles
    uint32_t ple_window = 4096;
    /// Enable adaptive PLE window sizing
    bool adaptive = true;
};

/// Per-vCPU runtime statistics
struct VcpuCpuStats {
    uint32_t vcpu_id = 0;
    /// Total user-mode time (nanoseconds)
    uint64_t user_time_ns = 0;
    /// Total system/kernel time (nanoseconds)
    uint64_t system_time_ns = 0;
    /// Total idle/halted time (nanoseconds)
    uint64_t idle_time_ns = 0;
    /// Number of VM exits
    uint64_t vm_exits = 0;
    /// Number of pause-loop exits
    uint64_t ple_exits = 0;
    /// Core the vCPU is pinned to (-1 = not pinned)
    int32_t pinned_core = -1;
    /// Current scheduling priority
    VcpuPriority priority = VcpuPriority::Normal;
};

/// Aggregate CPU statistics across all vCPUs
struct CpuStats {
    std::vector<VcpuCpuStats> per_vcpu;
    uint64_t total_vm_exits = 0;
    uint64_t total_ple_exits = 0;
    double avg_exit_latency_us = 0.0;
};

/// CPU optimization manager
///
/// Handles vCPU thread pinning, core topology detection,
/// pause-loop exiting configuration, TSC scaling, CPUID filtering,
/// and vCPU scheduling priority management.
class CpuOptimizer {
public:
    CpuOptimizer();
    ~CpuOptimizer();

    // Non-copyable
    CpuOptimizer(const CpuOptimizer&) = delete;
    CpuOptimizer& operator=(const CpuOptimizer&) = delete;

    /// Detect host CPU topology (cores, threads, hybrid layout)
    CoreTopology detect_topology();

    /// Pin a vCPU thread to a specific host core
    /// @param vcpu_id  vCPU identifier
    /// @param core_id  host core to pin to
    /// @param thread   the vCPU's std::thread (must be joinable)
    /// @return true on success
    bool pin_vcpu(uint32_t vcpu_id, uint32_t core_id, std::thread& thread);

    /// Unpin a vCPU thread (remove affinity constraint)
    bool unpin_vcpu(uint32_t vcpu_id);

    /// Set the scheduling priority of a vCPU thread
    bool set_priority(uint32_t vcpu_id, VcpuPriority priority, std::thread& thread);

    /// Auto-assign vCPUs to cores based on topology heuristics
    /// Prefers P-cores for vCPUs on hybrid architectures.
    /// @param vcpu_threads map of vcpu_id -> thread reference
    /// @param topology     pre-detected topology (or will detect if empty)
    /// @return number of vCPUs successfully pinned
    uint32_t auto_pin(std::vector<std::pair<uint32_t, std::thread*>>& vcpu_threads,
                      const CoreTopology& topology);

    /// Configure TSC scaling for the VM
    TscConfig configure_tsc();

    /// Set a custom TSC configuration
    void set_tsc_config(const TscConfig& config);
    const TscConfig& tsc_config() const { return tsc_config_; }

    /// Configure pause-loop exiting parameters
    void set_ple_config(const PleConfig& config);
    const PleConfig& ple_config() const { return ple_config_; }

    /// Add a CPUID filter/override entry
    void add_cpuid_entry(const CpuidEntry& entry);

    /// Remove a CPUID filter by function/index
    void remove_cpuid_entry(uint32_t function, uint32_t index = 0);

    /// Get all CPUID filter entries
    std::vector<CpuidEntry> cpuid_entries() const;

    /// Apply a standard CPUID filter set (hides VMX, SGX, etc.)
    void apply_default_cpuid_filters();

    /// Record a VM exit for statistics
    void record_vm_exit(uint32_t vcpu_id, double latency_us);

    /// Record a PLE exit for statistics
    void record_ple_exit(uint32_t vcpu_id);

    /// Record time spent in different modes
    void record_time(uint32_t vcpu_id, uint64_t user_ns, uint64_t system_ns, uint64_t idle_ns);

    /// Get aggregate CPU statistics
    CpuStats get_stats() const;

    /// Reset all statistics
    void reset_stats();

    /// Get the cached topology (call detect_topology first)
    const CoreTopology& topology() const { return topology_; }

private:
    /// Read CPUID leaf (platform-specific)
    void cpuid(uint32_t leaf, uint32_t subleaf,
               uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx);

    /// Detect TSC frequency from hardware
    uint64_t detect_tsc_frequency();

    /// Set native thread priority (platform-specific)
    bool set_native_priority(std::thread& thread, VcpuPriority priority);

    CoreTopology topology_;
    TscConfig tsc_config_;
    PleConfig ple_config_;

    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, VcpuCpuStats> vcpu_stats_;
    std::vector<CpuidEntry> cpuid_entries_;

    // Exit latency tracking (exponential moving average)
    double total_exit_latency_ = 0.0;
    uint64_t total_exit_count_ = 0;
};

} // namespace rex::vmm
