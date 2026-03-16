#pragma once

#include "rex/hal/hypervisor.h"
#include <memory>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)

namespace rex::hal {

// Forward declare the internal implementation
struct HvfArm64VcpuImpl;

/// IVcpu adapter for HVF ARM64
/// Maps the x86-centric IVcpu interface to ARM64 registers:
///   - X86Regs.rax..r15 → ARM64 X0..X15, rip→PC, rflags→CPSR, rsp→SP
///   - X86Sregs → ARM64 system registers (SCTLR, TCR, TTBR, etc.)
///   - inject_interrupt → HVF ARM64 interrupt injection
class HvfArm64VcpuAdapter : public IVcpu {
public:
    HvfArm64VcpuAdapter(VcpuId id);
    ~HvfArm64VcpuAdapter() override;

    HalResult<VcpuExit> run() override;
    VcpuId id() const override;
    HalResult<X86Regs> get_regs() const override;
    HalResult<void> set_regs(const X86Regs& regs) override;
    HalResult<X86Sregs> get_sregs() const override;
    HalResult<void> set_sregs(const X86Sregs& sregs) override;
    HalResult<void> inject_interrupt(uint32_t irq) override;
    HalResult<uint64_t> get_msr(uint32_t index) const override;
    HalResult<void> set_msr(uint32_t index, uint64_t value) override;

    bool is_valid() const;

private:
    std::unique_ptr<HvfArm64VcpuImpl> impl_;
};

/// IMemoryManager for HVF ARM64 (uses hv_vm_map)
class HvfArm64MemoryManager : public IMemoryManager {
public:
    HalResult<void> map_region(const MemoryRegion& region) override;
    HalResult<void> unmap_region(uint32_t slot) override;
    HalResult<HVA> gpa_to_hva(GPA gpa) const override;
    std::vector<MemoryRegion> get_regions() const override;

private:
    std::vector<MemoryRegion> regions_;
};

/// IHypervisor implementation for HVF on Apple Silicon
class HvfArm64Hypervisor : public IHypervisor {
public:
    HvfArm64Hypervisor();
    ~HvfArm64Hypervisor() override;

    std::string name() const override { return "HVF-ARM64"; }
    bool is_available() const override;
    HalResult<void> initialize() override;
    HalResult<void> create_vm(const VmConfig& config) override;
    HalResult<std::unique_ptr<IVcpu>> create_vcpu(VcpuId id) override;
    IMemoryManager& memory_manager() override;
    HalResult<void> create_irqchip() override;
    HalResult<void> set_irq_line(uint32_t irq, bool level) override;
    int api_version() const override;

private:
    std::unique_ptr<HvfArm64MemoryManager> mem_mgr_;
    VmConfig config_;
    bool vm_created_ = false;
};

} // namespace rex::hal

#endif // __APPLE__ && __aarch64__
