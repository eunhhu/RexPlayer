#pragma once

#include "rex/hal/types.h"

namespace rex::hal {

/// Interface for a virtual CPU
class IVcpu {
public:
    virtual ~IVcpu() = default;

    /// Run the vCPU until it exits
    virtual HalResult<VcpuExit> run() = 0;

    /// Get the vCPU identifier
    virtual VcpuId id() const = 0;

    /// Get general-purpose registers
    virtual HalResult<X86Regs> get_regs() const = 0;

    /// Set general-purpose registers
    virtual HalResult<void> set_regs(const X86Regs& regs) = 0;

    /// Get special registers (segment, control, etc.)
    virtual HalResult<X86Sregs> get_sregs() const = 0;

    /// Set special registers
    virtual HalResult<void> set_sregs(const X86Sregs& sregs) = 0;

    /// Inject an interrupt into the vCPU
    virtual HalResult<void> inject_interrupt(uint32_t irq) = 0;

    /// Get a model-specific register (MSR)
    virtual HalResult<uint64_t> get_msr(uint32_t index) const = 0;

    /// Set a model-specific register (MSR)
    virtual HalResult<void> set_msr(uint32_t index, uint64_t value) = 0;
};

} // namespace rex::hal
