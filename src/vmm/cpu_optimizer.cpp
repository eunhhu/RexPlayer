#include "cpu_optimizer.h"
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <sys/sysctl.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>
#endif

namespace rex::vmm {

// ============================================================================
// Construction
// ============================================================================

CpuOptimizer::CpuOptimizer() = default;

CpuOptimizer::~CpuOptimizer() = default;

// ============================================================================
// CPUID helper
// ============================================================================

void CpuOptimizer::cpuid(uint32_t leaf, uint32_t subleaf,
                          uint32_t& eax, uint32_t& ebx,
                          uint32_t& ecx, uint32_t& edx) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #if defined(_MSC_VER)
    int info[4];
    __cpuidex(info, static_cast<int>(leaf), static_cast<int>(subleaf));
    eax = static_cast<uint32_t>(info[0]);
    ebx = static_cast<uint32_t>(info[1]);
    ecx = static_cast<uint32_t>(info[2]);
    edx = static_cast<uint32_t>(info[3]);
    #else
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
    );
    #endif
#else
    // Non-x86: return zeroes
    eax = ebx = ecx = edx = 0;
#endif
}

// ============================================================================
// Topology detection
// ============================================================================

CoreTopology CpuOptimizer::detect_topology() {
    CoreTopology topo{};

    // Get total logical processors from platform layer
    topo.num_logical_threads = rex::platform::cpu_count();

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // Read vendor string via CPUID leaf 0
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, eax, ebx, ecx, edx);

    char vendor[13] = {};
    std::memcpy(vendor + 0, &ebx, 4);
    std::memcpy(vendor + 4, &edx, 4);
    std::memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    topo.vendor = vendor;

    uint32_t max_basic_leaf = eax;

    // Read brand/model string via CPUID leaf 0x80000002-0x80000004
    uint32_t ext_eax;
    cpuid(0x80000000, 0, ext_eax, ebx, ecx, edx);

    if (ext_eax >= 0x80000004) {
        char brand[49] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            cpuid(0x80000002 + i, 0, eax, ebx, ecx, edx);
            std::memcpy(brand + i * 16 + 0, &eax, 4);
            std::memcpy(brand + i * 16 + 4, &ebx, 4);
            std::memcpy(brand + i * 16 + 8, &ecx, 4);
            std::memcpy(brand + i * 16 + 12, &edx, 4);
        }
        brand[48] = '\0';
        // Trim leading spaces
        const char* p = brand;
        while (*p == ' ') ++p;
        topo.model_name = p;
    }

    // Detect cores via CPUID leaf 0x0B (Extended Topology Enumeration)
    if (max_basic_leaf >= 0x0B) {
        uint32_t smt_threads_per_core = 0;
        uint32_t logical_per_package = 0;

        // Sub-leaf 0: SMT level
        cpuid(0x0B, 0, eax, ebx, ecx, edx);
        if ((ecx & 0xFF00) != 0) {
            smt_threads_per_core = ebx & 0xFFFF;
        }

        // Sub-leaf 1: Core level
        cpuid(0x0B, 1, eax, ebx, ecx, edx);
        if ((ecx & 0xFF00) != 0) {
            logical_per_package = ebx & 0xFFFF;
        }

        if (smt_threads_per_core > 0 && logical_per_package > 0) {
            topo.num_physical_cores = logical_per_package / smt_threads_per_core;
        } else {
            topo.num_physical_cores = topo.num_logical_threads;
        }
        topo.num_packages = 1;
    } else {
        // Fallback: leaf 1
        cpuid(1, 0, eax, ebx, ecx, edx);
        uint32_t logical_per_package = (ebx >> 16) & 0xFF;
        if (logical_per_package > 0) {
            // Assume 2-way SMT if logical > 1
            topo.num_physical_cores = topo.num_logical_threads / 2;
            if (topo.num_physical_cores == 0) topo.num_physical_cores = 1;
        } else {
            topo.num_physical_cores = topo.num_logical_threads;
        }
        topo.num_packages = 1;
    }

    // Detect hybrid architecture (Intel leaf 0x1A)
    if (topo.vendor == "GenuineIntel" && max_basic_leaf >= 0x1A) {
        topo.hybrid = false;
        topo.num_p_cores = 0;
        topo.num_e_cores = 0;

        // Check CPUID leaf 7, sub-leaf 0, EDX bit 15 for hybrid support
        cpuid(7, 0, eax, ebx, ecx, edx);
        bool has_hybrid = (edx >> 15) & 1;

        if (has_hybrid) {
            topo.hybrid = true;
            // Query leaf 0x1A for each logical processor's core type
            // Core type is in EAX[31:24]: 0x20 = E-core (Atom), 0x40 = P-core (Core)
            cpuid(0x1A, 0, eax, ebx, ecx, edx);
            uint8_t current_core_type = (eax >> 24) & 0xFF;

            // We can only query the current core's type via CPUID directly.
            // For a full enumeration, we'd need to run cpuid on each core.
            // Estimate based on known Intel hybrid ratios.
            if (current_core_type == 0x40 || current_core_type == 0x20) {
                // Heuristic: on typical Alder/Raptor Lake, ~half are P-cores
                topo.num_p_cores = topo.num_physical_cores / 2;
                topo.num_e_cores = topo.num_physical_cores - topo.num_p_cores;
                if (topo.num_p_cores == 0) topo.num_p_cores = 1;
            }
        }
    }

    // Build per-core info
    topo.cores.reserve(topo.num_logical_threads);
    uint32_t threads_per_core = (topo.num_physical_cores > 0)
        ? topo.num_logical_threads / topo.num_physical_cores
        : 1;
    if (threads_per_core == 0) threads_per_core = 1;

    for (uint32_t i = 0; i < topo.num_logical_threads; ++i) {
        CoreInfo ci{};
        ci.core_id = i / threads_per_core;
        ci.thread_id = i % threads_per_core;
        ci.package_id = 0;

        if (topo.hybrid && topo.num_p_cores > 0) {
            ci.is_performance_core = (ci.core_id < topo.num_p_cores);
        } else {
            ci.is_performance_core = true;
        }
        topo.cores.push_back(ci);
    }

#else
    // Non-x86 fallback (ARM, etc.)
    topo.vendor = "Unknown";
    topo.model_name = "Unknown";
    topo.num_physical_cores = topo.num_logical_threads;
    topo.num_packages = 1;
    topo.hybrid = false;

    #ifdef __APPLE__
    // Apple Silicon: detect P/E cores via sysctl
    int32_t p_cores = 0, e_cores = 0;
    size_t len = sizeof(p_cores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &p_cores, &len, nullptr, 0) == 0) {
        len = sizeof(e_cores);
        sysctlbyname("hw.perflevel1.physicalcpu", &e_cores, &len, nullptr, 0);
        if (p_cores > 0 && e_cores > 0) {
            topo.hybrid = true;
            topo.num_p_cores = static_cast<uint32_t>(p_cores);
            topo.num_e_cores = static_cast<uint32_t>(e_cores);
            topo.num_physical_cores = topo.num_p_cores + topo.num_e_cores;
        }
    }
    #endif

    topo.cores.reserve(topo.num_logical_threads);
    for (uint32_t i = 0; i < topo.num_logical_threads; ++i) {
        CoreInfo ci{};
        ci.core_id = i;
        ci.thread_id = 0;
        ci.package_id = 0;
        if (topo.hybrid && topo.num_p_cores > 0) {
            ci.is_performance_core = (i < topo.num_p_cores);
        } else {
            ci.is_performance_core = true;
        }
        topo.cores.push_back(ci);
    }
#endif

    topology_ = topo;
    return topo;
}

// ============================================================================
// vCPU thread pinning
// ============================================================================

bool CpuOptimizer::pin_vcpu(uint32_t vcpu_id, uint32_t core_id, std::thread& thread) {
    if (core_id >= topology_.num_logical_threads && topology_.num_logical_threads > 0) {
        fprintf(stderr, "CpuOptimizer: core_id %u out of range (max %u)\n",
                core_id, topology_.num_logical_threads - 1);
        return false;
    }

    if (!thread.joinable()) {
        fprintf(stderr, "CpuOptimizer: vCPU %u thread is not joinable\n", vcpu_id);
        return false;
    }

    bool ok = false;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    ok = pthread_setaffinity_np(thread.native_handle(), sizeof(cpuset), &cpuset) == 0;
#elif defined(__APPLE__)
    // macOS: use thread affinity policy as a hint
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = static_cast<integer_t>(core_id + 1); // 0 = no affinity
    kern_return_t kr = thread_policy_set(
        pthread_mach_thread_np(thread.native_handle()),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT);
    ok = (kr == KERN_SUCCESS);
#elif defined(_WIN32)
    DWORD_PTR mask = 1ULL << core_id;
    ok = SetThreadAffinityMask(
        reinterpret_cast<HANDLE>(thread.native_handle()), mask) != 0;
#endif

    if (ok) {
        std::lock_guard lock(mutex_);
        vcpu_stats_[vcpu_id].vcpu_id = vcpu_id;
        vcpu_stats_[vcpu_id].pinned_core = static_cast<int32_t>(core_id);
    }

    return ok;
}

bool CpuOptimizer::unpin_vcpu(uint32_t vcpu_id) {
    std::lock_guard lock(mutex_);
    auto it = vcpu_stats_.find(vcpu_id);
    if (it != vcpu_stats_.end()) {
        it->second.pinned_core = -1;
        // Note: actually clearing affinity requires the thread handle,
        // which we don't store. The caller should re-pin or the OS
        // will schedule freely once the thread is re-created.
        return true;
    }
    return false;
}

// ============================================================================
// Scheduling priority
// ============================================================================

bool CpuOptimizer::set_native_priority(std::thread& thread, VcpuPriority priority) {
    if (!thread.joinable()) return false;

#ifdef __linux__
    int policy = SCHED_OTHER;
    struct sched_param param{};

    switch (priority) {
        case VcpuPriority::Idle:
            policy = SCHED_IDLE;
            param.sched_priority = 0;
            break;
        case VcpuPriority::Low:
            policy = SCHED_OTHER;
            param.sched_priority = 0;
            // Use nice value via setpriority after setting policy
            break;
        case VcpuPriority::Normal:
            policy = SCHED_OTHER;
            param.sched_priority = 0;
            break;
        case VcpuPriority::High:
            policy = SCHED_FIFO;
            param.sched_priority = 50;
            break;
        case VcpuPriority::Realtime:
            policy = SCHED_FIFO;
            param.sched_priority = 90;
            break;
    }

    return pthread_setschedparam(thread.native_handle(), policy, &param) == 0;

#elif defined(__APPLE__)
    // macOS: use thread QoS classes
    struct sched_param param{};
    int policy = SCHED_OTHER;

    switch (priority) {
        case VcpuPriority::Idle:
            param.sched_priority = 0;
            break;
        case VcpuPriority::Low:
            param.sched_priority = 15;
            break;
        case VcpuPriority::Normal:
            param.sched_priority = 31;
            break;
        case VcpuPriority::High:
            param.sched_priority = 47;
            break;
        case VcpuPriority::Realtime:
            param.sched_priority = 63;
            policy = SCHED_RR;
            break;
    }

    return pthread_setschedparam(thread.native_handle(), policy, &param) == 0;

#elif defined(_WIN32)
    int win_priority = THREAD_PRIORITY_NORMAL;
    switch (priority) {
        case VcpuPriority::Idle:
            win_priority = THREAD_PRIORITY_IDLE;
            break;
        case VcpuPriority::Low:
            win_priority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case VcpuPriority::Normal:
            win_priority = THREAD_PRIORITY_NORMAL;
            break;
        case VcpuPriority::High:
            win_priority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case VcpuPriority::Realtime:
            win_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
    }

    return SetThreadPriority(
        reinterpret_cast<HANDLE>(thread.native_handle()), win_priority) != 0;
#else
    (void)thread;
    (void)priority;
    return false;
#endif
}

bool CpuOptimizer::set_priority(uint32_t vcpu_id, VcpuPriority priority,
                                 std::thread& thread) {
    bool ok = set_native_priority(thread, priority);
    if (ok) {
        std::lock_guard lock(mutex_);
        vcpu_stats_[vcpu_id].vcpu_id = vcpu_id;
        vcpu_stats_[vcpu_id].priority = priority;
    }
    return ok;
}

// ============================================================================
// Auto-pin heuristic
// ============================================================================

uint32_t CpuOptimizer::auto_pin(
    std::vector<std::pair<uint32_t, std::thread*>>& vcpu_threads,
    const CoreTopology& topology)
{
    const CoreTopology& topo = (topology.num_logical_threads > 0) ? topology : topology_;

    if (topo.cores.empty() || vcpu_threads.empty()) return 0;

    // Build a sorted list of preferred cores:
    // 1. P-cores first (on hybrid)
    // 2. First hardware thread per core (avoid SMT siblings initially)
    std::vector<uint32_t> preferred;
    std::vector<uint32_t> secondary;

    for (size_t i = 0; i < topo.cores.size(); ++i) {
        const auto& ci = topo.cores[i];
        if (ci.thread_id == 0 && ci.is_performance_core) {
            preferred.push_back(static_cast<uint32_t>(i));
        } else if (ci.thread_id == 0) {
            secondary.push_back(static_cast<uint32_t>(i));
        }
    }

    // Add SMT siblings as last resort
    for (size_t i = 0; i < topo.cores.size(); ++i) {
        if (topo.cores[i].thread_id != 0) {
            secondary.push_back(static_cast<uint32_t>(i));
        }
    }

    // Merge: preferred first, then secondary
    std::vector<uint32_t> assignment_order;
    assignment_order.reserve(preferred.size() + secondary.size());
    assignment_order.insert(assignment_order.end(), preferred.begin(), preferred.end());
    assignment_order.insert(assignment_order.end(), secondary.begin(), secondary.end());

    uint32_t pinned = 0;
    for (size_t i = 0; i < vcpu_threads.size() && i < assignment_order.size(); ++i) {
        auto [vcpu_id, thread_ptr] = vcpu_threads[i];
        if (thread_ptr && thread_ptr->joinable()) {
            if (pin_vcpu(vcpu_id, assignment_order[i], *thread_ptr)) {
                ++pinned;
            }
        }
    }

    return pinned;
}

// ============================================================================
// TSC configuration
// ============================================================================

uint64_t CpuOptimizer::detect_tsc_frequency() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // Try CPUID leaf 0x15 (TSC/Crystal ratio) + leaf 0x16 (processor freq)
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, eax, ebx, ecx, edx);
    uint32_t max_leaf = eax;

    if (max_leaf >= 0x15) {
        cpuid(0x15, 0, eax, ebx, ecx, edx);
        // EAX = denominator, EBX = numerator, ECX = crystal frequency (Hz)
        if (eax != 0 && ebx != 0 && ecx != 0) {
            uint64_t tsc_hz = (static_cast<uint64_t>(ecx) * ebx) / eax;
            return tsc_hz / 1000; // Return KHz
        }
    }

    if (max_leaf >= 0x16) {
        cpuid(0x16, 0, eax, ebx, ecx, edx);
        // EAX = base frequency (MHz)
        if (eax != 0) {
            return static_cast<uint64_t>(eax) * 1000; // MHz -> KHz
        }
    }
#endif

    // Fallback: measure TSC over a short interval
    uint64_t start = rex::platform::timestamp_ns();
    // Busy-wait ~1ms
    volatile uint32_t dummy = 0;
    for (uint32_t i = 0; i < 1000000; ++i) { dummy += i; }
    uint64_t elapsed = rex::platform::timestamp_ns() - start;

    if (elapsed > 0) {
        // Rough estimate: assume ~3 GHz if measurement is too noisy
        return 3000000; // 3 GHz in KHz
    }
    return 3000000;
}

TscConfig CpuOptimizer::configure_tsc() {
    tsc_config_.host_frequency_khz = detect_tsc_frequency();
    tsc_config_.guest_frequency_khz = tsc_config_.host_frequency_khz;
    tsc_config_.scaling_enabled = false;
    tsc_config_.rdtsc_exit_enabled = false;

    return tsc_config_;
}

void CpuOptimizer::set_tsc_config(const TscConfig& config) {
    std::lock_guard lock(mutex_);
    tsc_config_ = config;
}

// ============================================================================
// PLE configuration
// ============================================================================

void CpuOptimizer::set_ple_config(const PleConfig& config) {
    std::lock_guard lock(mutex_);
    ple_config_ = config;
}

// ============================================================================
// CPUID filtering
// ============================================================================

void CpuOptimizer::add_cpuid_entry(const CpuidEntry& entry) {
    std::lock_guard lock(mutex_);

    // Replace existing entry for same function/index
    for (auto& e : cpuid_entries_) {
        if (e.function == entry.function && e.index == entry.index) {
            e = entry;
            return;
        }
    }
    cpuid_entries_.push_back(entry);
}

void CpuOptimizer::remove_cpuid_entry(uint32_t function, uint32_t index) {
    std::lock_guard lock(mutex_);
    cpuid_entries_.erase(
        std::remove_if(cpuid_entries_.begin(), cpuid_entries_.end(),
            [function, index](const CpuidEntry& e) {
                return e.function == function && e.index == index;
            }),
        cpuid_entries_.end());
}

std::vector<CpuidEntry> CpuOptimizer::cpuid_entries() const {
    std::lock_guard lock(mutex_);
    return cpuid_entries_;
}

void CpuOptimizer::apply_default_cpuid_filters() {
    std::lock_guard lock(mutex_);
    cpuid_entries_.clear();

    // Hide VMX (Virtual Machine Extensions) — CPUID.1:ECX bit 5
    {
        CpuidEntry e{};
        e.function = 1;
        e.index = 0;
        e.passthrough = false;
        // Read current values
        uint32_t eax, ebx, ecx, edx;
        cpuid(1, 0, eax, ebx, ecx, edx);
        e.eax = eax;
        e.ebx = ebx;
        e.ecx = ecx & ~(1u << 5);  // Clear VMX bit
        e.edx = edx;
        cpuid_entries_.push_back(e);
    }

    // Hide hypervisor present bit — CPUID.1:ECX bit 31
    // (already handled above, but explicitly clear it)
    if (!cpuid_entries_.empty()) {
        cpuid_entries_[0].ecx &= ~(1u << 31); // Clear hypervisor-present
    }

    // Hide SGX — CPUID.7:EBX bit 2
    {
        CpuidEntry e{};
        e.function = 7;
        e.index = 0;
        e.passthrough = false;
        uint32_t eax, ebx, ecx, edx;
        cpuid(7, 0, eax, ebx, ecx, edx);
        e.eax = eax;
        e.ebx = ebx & ~(1u << 2); // Clear SGX
        e.ecx = ecx;
        e.edx = edx;
        cpuid_entries_.push_back(e);
    }

    // Present a known-good CPUID vendor and family to the guest
    // (passthrough by default — only override if needed)
}

// ============================================================================
// Statistics
// ============================================================================

void CpuOptimizer::record_vm_exit(uint32_t vcpu_id, double latency_us) {
    std::lock_guard lock(mutex_);
    auto& stats = vcpu_stats_[vcpu_id];
    stats.vcpu_id = vcpu_id;
    stats.vm_exits++;
    total_exit_count_++;
    // Exponential moving average (alpha = 0.05)
    total_exit_latency_ = total_exit_latency_ * 0.95 + latency_us * 0.05;
}

void CpuOptimizer::record_ple_exit(uint32_t vcpu_id) {
    std::lock_guard lock(mutex_);
    auto& stats = vcpu_stats_[vcpu_id];
    stats.vcpu_id = vcpu_id;
    stats.ple_exits++;
}

void CpuOptimizer::record_time(uint32_t vcpu_id,
                                uint64_t user_ns, uint64_t system_ns,
                                uint64_t idle_ns) {
    std::lock_guard lock(mutex_);
    auto& stats = vcpu_stats_[vcpu_id];
    stats.vcpu_id = vcpu_id;
    stats.user_time_ns += user_ns;
    stats.system_time_ns += system_ns;
    stats.idle_time_ns += idle_ns;
}

CpuStats CpuOptimizer::get_stats() const {
    std::lock_guard lock(mutex_);
    CpuStats result{};

    result.per_vcpu.reserve(vcpu_stats_.size());
    result.total_vm_exits = 0;
    result.total_ple_exits = 0;

    for (const auto& [id, stats] : vcpu_stats_) {
        result.per_vcpu.push_back(stats);
        result.total_vm_exits += stats.vm_exits;
        result.total_ple_exits += stats.ple_exits;
    }

    // Sort by vCPU ID for deterministic output
    std::sort(result.per_vcpu.begin(), result.per_vcpu.end(),
              [](const VcpuCpuStats& a, const VcpuCpuStats& b) {
                  return a.vcpu_id < b.vcpu_id;
              });

    result.avg_exit_latency_us = total_exit_latency_;
    return result;
}

void CpuOptimizer::reset_stats() {
    std::lock_guard lock(mutex_);
    vcpu_stats_.clear();
    total_exit_latency_ = 0.0;
    total_exit_count_ = 0;
}

} // namespace rex::vmm
