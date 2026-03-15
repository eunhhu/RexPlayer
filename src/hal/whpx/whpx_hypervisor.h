#pragma once

#include "rex/hal/hypervisor.h"
#include <vector>

namespace rex::hal {

class WhpxVcpu : public IVcpu {
public:
    WhpxVcpu(VcpuId id);
    ~WhpxVcpu() override;

    HalResult<VcpuExit> run() override;
    VcpuId id() const override { return id_; }
    HalResult<X86Regs> get_regs() const override;
    HalResult<void> set_regs(const X86Regs& regs) override;
    HalResult<X86Sregs> get_sregs() const override;
    HalResult<void> set_sregs(const X86Sregs& sregs) override;
    HalResult<void> inject_interrupt(uint32_t irq) override;
    HalResult<uint64_t> get_msr(uint32_t index) const override;
    HalResult<void> set_msr(uint32_t index, uint64_t value) override;

private:
    VcpuId id_;
    void* partition_ = nullptr;
};

class WhpxMemoryManager : public IMemoryManager {
public:
    explicit WhpxMemoryManager(void* partition);

    HalResult<void> map_region(const MemoryRegion& region) override;
    HalResult<void> unmap_region(uint32_t slot) override;
    HalResult<HVA> gpa_to_hva(GPA gpa) const override;
    std::vector<MemoryRegion> get_regions() const override;

private:
    void* partition_;
    std::vector<MemoryRegion> regions_;
};

class WhpxHypervisor : public IHypervisor {
public:
    WhpxHypervisor();
    ~WhpxHypervisor() override;

    std::string name() const override { return "WHPX"; }
    bool is_available() const override;
    HalResult<void> initialize() override;
    HalResult<void> create_vm(const VmConfig& config) override;
    HalResult<std::unique_ptr<IVcpu>> create_vcpu(VcpuId id) override;
    IMemoryManager& memory_manager() override;
    HalResult<void> create_irqchip() override;
    HalResult<void> set_irq_line(uint32_t irq, bool level) override;
    int api_version() const override;

private:
    void* partition_ = nullptr;
    std::unique_ptr<WhpxMemoryManager> mem_mgr_;
    VmConfig config_;
};

} // namespace rex::hal
