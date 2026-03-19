#pragma once

#include "rex/hal/types.h"
#include "rex/hal/vcpu.h"
#include "rex/hal/memory.h"
#include <memory>
#include <string>

namespace rex::hal {

/// Configuration for creating a VM
struct VmConfig {
    uint32_t num_vcpus = 1;
    MemSize ram_size = 512ULL * 1024 * 1024; // 512 MB default
    bool enable_irqchip = true;              // in-kernel interrupt controller
};

/// Interface for the hypervisor abstraction layer
///
/// Each platform (KVM, WHPX, HVF) implements this interface.
class IHypervisor {
public:
    virtual ~IHypervisor() = default;

    /// Get the hypervisor backend name (e.g., "KVM", "WHPX", "HVF")
    virtual std::string name() const = 0;

    /// Check if the hypervisor is available on the current system
    virtual bool is_available() const = 0;

    /// Initialize the hypervisor (open device, check capabilities)
    virtual HalResult<void> initialize() = 0;

    /// Create a new virtual machine
    virtual HalResult<void> create_vm(const VmConfig& config) = 0;

    /// Create a vCPU for the VM
    virtual HalResult<std::unique_ptr<IVcpu>> create_vcpu(VcpuId id) = 0;

    /// Get the memory manager for guest physical memory
    virtual IMemoryManager& memory_manager() = 0;

    /// Set up the in-kernel interrupt controller (APIC/GIC)
    virtual HalResult<void> create_irqchip() = 0;

    /// Inject an interrupt into the VM's interrupt controller
    virtual HalResult<void> set_irq_line(uint32_t irq, bool level) = 0;

    /// Get the API version of the hypervisor
    virtual int api_version() const = 0;
};

/// Factory function: creates the appropriate hypervisor for the current platform
std::unique_ptr<IHypervisor> create_hypervisor();

} // namespace rex::hal
