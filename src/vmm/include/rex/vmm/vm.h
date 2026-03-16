#pragma once

#include "rex/hal/hypervisor.h"
#include "rex/vmm/device_manager.h"
#include "rex/vmm/memory_manager.h"
#include "rex/vmm/boot.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace rex::vmm {

/// VM state
enum class VmState {
    Created,
    Running,
    Paused,
    Stopped,
};

/// Configuration for creating a VM
struct VmCreateConfig {
#if defined(__aarch64__)
    uint32_t num_vcpus = 1;
#else
    uint32_t num_vcpus = 2;
#endif
    rex::hal::MemSize ram_size = 2ULL * 1024 * 1024 * 1024; // 2 GB
    BootParams boot;
};

/// The main VM class — owns the hypervisor, vCPUs, memory, and devices
class Vm {
public:
    Vm();
    ~Vm();

    // Non-copyable
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    /// Create and initialize the VM
    rex::hal::HalResult<void> create(const VmCreateConfig& config);

    /// Start all vCPU threads
    rex::hal::HalResult<void> start();

    /// Pause all vCPUs
    void pause();

    /// Resume from paused state
    void resume();

    /// Stop the VM (signals vCPU threads to exit)
    void stop();

    /// Get current VM state
    VmState state() const { return state_.load(); }

    /// Access the device manager for registering devices
    DeviceManager& device_manager() { return device_mgr_; }

    /// Access the memory manager
    MemoryManager& memory_manager() { return *mem_mgr_; }
    const MemoryManager& memory_manager() const { return *mem_mgr_; }

    /// Access vCPUs (for snapshot save/restore)
    const std::vector<std::unique_ptr<rex::hal::IVcpu>>& vcpus() const { return vcpus_; }
    std::vector<std::unique_ptr<rex::hal::IVcpu>>& vcpus() { return vcpus_; }

    /// Get the VM creation config
    const VmCreateConfig& config() const { return config_; }

    /// Access the hypervisor (for snapshot restore — creating new vCPUs, etc.)
    rex::hal::IHypervisor* hypervisor() { return hypervisor_.get(); }

private:
    friend class SnapshotManager;
    /// vCPU thread entry point
    void vcpu_loop(rex::hal::IVcpu* vcpu);

    std::unique_ptr<rex::hal::IHypervisor> hypervisor_;
    std::unique_ptr<MemoryManager> mem_mgr_;
    DeviceManager device_mgr_;

    std::vector<std::unique_ptr<rex::hal::IVcpu>> vcpus_;
    std::vector<std::thread> vcpu_threads_;

    std::atomic<VmState> state_{VmState::Created};
    std::atomic<bool> should_stop_{false};

    VmCreateConfig config_;
};

} // namespace rex::vmm
