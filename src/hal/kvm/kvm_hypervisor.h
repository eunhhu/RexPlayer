#pragma once

#include "rex/hal/hypervisor.h"
#include <vector>

namespace rex::hal {

class KvmVcpu : public IVcpu {
public:
    KvmVcpu(int vm_fd, VcpuId id);
    ~KvmVcpu() override;

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
    int vcpu_fd_ = -1;
    VcpuId id_;
    void* kvm_run_ = nullptr;    // mmap'd kvm_run struct
    size_t kvm_run_size_ = 0;
};

class KvmMemoryManager : public IMemoryManager {
public:
    explicit KvmMemoryManager(int vm_fd);

    HalResult<void> map_region(const MemoryRegion& region) override;
    HalResult<void> unmap_region(uint32_t slot) override;
    HalResult<HVA> gpa_to_hva(GPA gpa) const override;
    std::vector<MemoryRegion> get_regions() const override;

private:
    int vm_fd_;
    std::vector<MemoryRegion> regions_;
};

class KvmHypervisor : public IHypervisor {
public:
    KvmHypervisor();
    ~KvmHypervisor() override;

    std::string name() const override { return "KVM"; }
    bool is_available() const override;
    HalResult<void> initialize() override;
    HalResult<void> create_vm(const VmConfig& config) override;
    HalResult<std::unique_ptr<IVcpu>> create_vcpu(VcpuId id) override;
    IMemoryManager& memory_manager() override;
    HalResult<void> create_irqchip() override;
    HalResult<void> set_irq_line(uint32_t irq, bool level) override;
    int api_version() const override;

private:
    int kvm_fd_ = -1;
    int vm_fd_ = -1;
    std::unique_ptr<KvmMemoryManager> mem_mgr_;
    VmConfig config_;
};

} // namespace rex::hal
