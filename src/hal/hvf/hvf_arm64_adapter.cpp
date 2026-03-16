#if defined(__APPLE__) && defined(__aarch64__)

#include "hvf_arm64_adapter.h"

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vcpu.h>
#include <Hypervisor/hv_vcpu_config.h>
#include <Hypervisor/hv_vcpu_types.h>
#include <Hypervisor/hv_vm.h>
#include <Hypervisor/hv_vm_types.h>
#include <cstring>
#include <algorithm>

namespace rex::hal {

// ============================================================================
// Internal vCPU handle (direct HVF API calls)
// ============================================================================

struct HvfArm64VcpuImpl {
    hv_vcpu_t handle = 0;
    hv_vcpu_exit_t* exit_info = nullptr;
    bool valid = false;
    bool created = false;
    VcpuId id;

    // Pending register state (set before run, applied on lazy create)
    X86Regs pending_regs{};
    X86Sregs pending_sregs{};
    bool has_pending_regs = false;
    bool has_pending_sregs = false;

    explicit HvfArm64VcpuImpl(VcpuId vid) : id(vid) {
        // Defer creation — HVF requires vCPU to be created on the thread that runs it
        valid = true; // Mark as "ready to create"
    }

    /// Actually create the vCPU (must be called from the run thread)
    bool ensure_created() {
        if (created) return true;
        hv_vcpu_config_t config = hv_vcpu_config_create();
        hv_return_t ret = hv_vcpu_create(&handle, &exit_info, config);
        if (config) os_release(config);
        if (ret != HV_SUCCESS) {
            valid = false;
            return false;
        }
        created = true;

        // Configure HCR_EL2 for proper guest trapping
        uint64_t hcr = 0;
        hv_vcpu_get_sys_reg(handle, HV_SYS_REG_HCR_EL2, &hcr);
        hcr |= (1ULL << 0);   // VM  — enable stage-2 translation (MMIO traps)
        hcr |= (1ULL << 1);   // SWIO — set/way invalidation override
        hcr |= (1ULL << 3);   // FMO — route FIQ to EL2
        hcr |= (1ULL << 4);   // IMO — route IRQ to EL2
        hcr |= (1ULL << 31);  // RW  — EL1 is AArch64
        hcr |= (1ULL << 27);  // TWI — trap WFI to EL2
        hv_vcpu_set_sys_reg(handle, HV_SYS_REG_HCR_EL2, hcr);

        fprintf(stderr, "vcpu %u: created on thread, HCR_EL2=0x%llx\n", id, hcr);

        // Apply any pending register state
        fprintf(stderr, "vcpu %u: has_pending_regs=%d has_pending_sregs=%d\n",
                id, has_pending_regs, has_pending_sregs);
        if (has_pending_regs) {
            fprintf(stderr, "vcpu %u: applying pending regs PC=0x%llx CPSR=0x%llx X0=0x%llx\n",
                    id, pending_regs.rip, pending_regs.rflags, pending_regs.rax);
            apply_regs(pending_regs);
            has_pending_regs = false;
        }
        if (has_pending_sregs) {
            apply_sregs(pending_sregs);
            has_pending_sregs = false;
        }
        return true;
    }

    void apply_regs(const X86Regs& regs) {
        const uint64_t* gp[] = {
            &regs.rax, &regs.rbx, &regs.rcx, &regs.rdx,
            &regs.rsi, &regs.rdi, &regs.rbp, &regs.rsp,
            &regs.r8,  &regs.r9,  &regs.r10, &regs.r11,
            &regs.r12, &regs.r13, &regs.r14, &regs.r15,
        };
        for (int i = 0; i < 16; ++i)
            hv_vcpu_set_reg(handle, static_cast<hv_reg_t>(HV_REG_X0 + i), *gp[i]);
        hv_vcpu_set_reg(handle, HV_REG_PC, regs.rip);
        hv_vcpu_set_reg(handle, HV_REG_CPSR, regs.rflags);
    }

    void apply_sregs(const X86Sregs& sregs) {
        hv_vcpu_set_sys_reg(handle, HV_SYS_REG_SCTLR_EL1, sregs.cr0);
        hv_vcpu_set_sys_reg(handle, HV_SYS_REG_TTBR0_EL1, sregs.cr3);
        hv_vcpu_set_sys_reg(handle, HV_SYS_REG_TCR_EL1, sregs.cr4);
        hv_vcpu_set_sys_reg(handle, HV_SYS_REG_MAIR_EL1, sregs.efer);
    }

    ~HvfArm64VcpuImpl() {
        if (created) hv_vcpu_destroy(handle);
    }
};

namespace {

constexpr uint64_t kSmcccRetNotSupported = ~0ULL;

bool complete_trapped_firmware_call(HvfArm64VcpuImpl& impl) {
    uint64_t pc = 0;
    if (hv_vcpu_get_reg(impl.handle, HV_REG_PC, &pc) != HV_SUCCESS) {
        return false;
    }
    if (hv_vcpu_set_reg(impl.handle, HV_REG_X0, kSmcccRetNotSupported) != HV_SUCCESS) {
        return false;
    }
    return hv_vcpu_set_reg(impl.handle, HV_REG_PC, pc + 4) == HV_SUCCESS;
}

} // namespace

// ============================================================================
// HvfArm64VcpuAdapter
// ============================================================================

HvfArm64VcpuAdapter::HvfArm64VcpuAdapter(VcpuId id)
    : impl_(std::make_unique<HvfArm64VcpuImpl>(id)) {}

HvfArm64VcpuAdapter::~HvfArm64VcpuAdapter() = default;

VcpuId HvfArm64VcpuAdapter::id() const {
    return impl_ ? impl_->id : 0;
}

bool HvfArm64VcpuAdapter::is_valid() const {
    return impl_ && impl_->valid;
}

HalResult<VcpuExit> HvfArm64VcpuAdapter::run() {
    if (!impl_ || !impl_->valid)
        return std::unexpected(HalError::NotInitialized);

    // Lazy create vCPU on the calling thread (HVF requirement)
    if (!impl_->ensure_created())
        return std::unexpected(HalError::InternalError);

    for (;;) {
        hv_return_t ret = hv_vcpu_run(impl_->handle);
        if (ret != HV_SUCCESS) {
            fprintf(stderr, "hv_vcpu_run failed: %d\n", ret);
            return std::unexpected(HalError::VcpuRunFailed);
        }

        VcpuExit exit{};
        auto* ei = impl_->exit_info;

        static uint64_t exit_count = 0;
        if (++exit_count <= 30) {
            uint64_t pc = 0;
            hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
            fprintf(stderr, "vcpu exit #%llu: reason=%d pc=0x%llx", exit_count, ei->reason, pc);
            if (ei->reason == HV_EXIT_REASON_EXCEPTION) {
                fprintf(stderr, " syndrome=0x%x ec=0x%x",
                        ei->exception.syndrome,
                        (ei->exception.syndrome >> 26) & 0x3F);
            }
            fprintf(stderr, "\n");
        }

        switch (ei->reason) {
            case HV_EXIT_REASON_EXCEPTION: {
                uint32_t syndrome = ei->exception.syndrome;
                uint8_t ec = (syndrome >> 26) & 0x3F;

                if (ec == 0x24 || ec == 0x25) { // Data abort → MMIO
                    exit.reason = VcpuExit::Reason::MmioAccess;
                    exit.mmio.address = ei->exception.physical_address;
                    bool isv = (syndrome >> 24) & 1;
                    bool wnr = (syndrome >> 6) & 1;
                    uint8_t sas = (syndrome >> 22) & 3;
                    exit.mmio.size = isv ? static_cast<uint8_t>(1 << sas) : 4;
                    exit.mmio.is_write = wnr;
                    if (wnr && isv) {
                        uint32_t srt = (syndrome >> 16) & 0x1F;
                        uint64_t val = 0;
                        hv_vcpu_get_reg(impl_->handle,
                            static_cast<hv_reg_t>(HV_REG_X0 + srt), &val);
                        exit.mmio.data = val;
                    }
                    // Advance PC past the faulting instruction (4 bytes for ARM64)
                    uint64_t pc = 0;
                    hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
                    hv_vcpu_set_reg(impl_->handle, HV_REG_PC, pc + 4);
                } else if (ec == 0x01) { // WFI/WFE → Hlt
                    exit.reason = VcpuExit::Reason::Hlt;
                } else if (ec == 0x16 || ec == 0x17) { // HVC/SMC
                    // Treat trapped firmware calls as "not supported" and
                    // advance past the instruction so guests are not killed
                    // before the VMM grows real PSCI/SMCCC handling.
                    if (!complete_trapped_firmware_call(*impl_)) {
                        return std::unexpected(HalError::InternalError);
                    }
                    continue;
                } else {
                    exit.reason = VcpuExit::Reason::Unknown;
                }
                break;
            }

            case HV_EXIT_REASON_VTIMER_ACTIVATED:
                exit.reason = VcpuExit::Reason::IrqWindowOpen;
                if (hv_vcpu_set_vtimer_mask(impl_->handle, true) != HV_SUCCESS) {
                    return std::unexpected(HalError::InternalError);
                }
                break;
            case HV_EXIT_REASON_CANCELED:
                exit.reason = VcpuExit::Reason::Unknown;
                break;
            default:
                exit.reason = VcpuExit::Reason::Unknown;
                break;
        }

        return exit;
    }
}

HalResult<X86Regs> HvfArm64VcpuAdapter::get_regs() const {
    if (!impl_ || !impl_->valid) return std::unexpected(HalError::NotInitialized);

    X86Regs regs{};
    uint64_t val;
    uint64_t* gp[] = {
        &regs.rax, &regs.rbx, &regs.rcx, &regs.rdx,
        &regs.rsi, &regs.rdi, &regs.rbp, &regs.rsp,
        &regs.r8,  &regs.r9,  &regs.r10, &regs.r11,
        &regs.r12, &regs.r13, &regs.r14, &regs.r15,
    };
    for (int i = 0; i < 16; ++i) {
        hv_vcpu_get_reg(impl_->handle,
            static_cast<hv_reg_t>(HV_REG_X0 + i), &val);
        *gp[i] = val;
    }
    hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &regs.rip);
    hv_vcpu_get_reg(impl_->handle, HV_REG_CPSR, &regs.rflags);
    return regs;
}

HalResult<void> HvfArm64VcpuAdapter::set_regs(const X86Regs& regs) {
    if (!impl_ || !impl_->valid) return std::unexpected(HalError::NotInitialized);

    if (!impl_->created) {
        // Defer — will be applied when vCPU is created on the run thread
        impl_->pending_regs = regs;
        impl_->has_pending_regs = true;
        return {};
    }

    impl_->apply_regs(regs);
    return {};
}

HalResult<X86Sregs> HvfArm64VcpuAdapter::get_sregs() const {
    X86Sregs sregs{};
    if (impl_ && impl_->valid) {
        hv_vcpu_get_sys_reg(impl_->handle, HV_SYS_REG_SCTLR_EL1, &sregs.cr0);
        hv_vcpu_get_sys_reg(impl_->handle, HV_SYS_REG_TTBR0_EL1, &sregs.cr3);
        hv_vcpu_get_sys_reg(impl_->handle, HV_SYS_REG_TCR_EL1, &sregs.cr4);
        hv_vcpu_get_sys_reg(impl_->handle, HV_SYS_REG_MAIR_EL1, &sregs.efer);
    }
    return sregs;
}

HalResult<void> HvfArm64VcpuAdapter::set_sregs(const X86Sregs& sregs) {
    if (!impl_ || !impl_->valid) return std::unexpected(HalError::NotInitialized);

    if (!impl_->created) {
        impl_->pending_sregs = sregs;
        impl_->has_pending_sregs = true;
        return {};
    }

    impl_->apply_sregs(sregs);
    return {};
}

HalResult<void> HvfArm64VcpuAdapter::inject_interrupt(uint32_t) {
    if (!impl_ || !impl_->valid) return std::unexpected(HalError::NotInitialized);
    hv_vcpu_set_pending_interrupt(impl_->handle, HV_INTERRUPT_TYPE_IRQ, true);
    return {};
}

HalResult<uint64_t> HvfArm64VcpuAdapter::get_msr(uint32_t index) const {
    if (!impl_ || !impl_->valid) return std::unexpected(HalError::NotInitialized);
    hv_sys_reg_t sys_reg;
    switch (index) {
        case 0x6000: sys_reg = HV_SYS_REG_SCTLR_EL1; break;
        case 0x6001: sys_reg = HV_SYS_REG_TTBR0_EL1; break;
        case 0x6002: sys_reg = HV_SYS_REG_TTBR1_EL1; break;
        case 0x6003: sys_reg = HV_SYS_REG_TCR_EL1; break;
        case 0x6004: sys_reg = HV_SYS_REG_MAIR_EL1; break;
        case 0x6005: sys_reg = HV_SYS_REG_VBAR_EL1; break;
        default: return std::unexpected(HalError::NotSupported);
    }
    uint64_t val = 0;
    if (hv_vcpu_get_sys_reg(impl_->handle, sys_reg, &val) != HV_SUCCESS)
        return std::unexpected(HalError::InternalError);
    return val;
}

HalResult<void> HvfArm64VcpuAdapter::set_msr(uint32_t index, uint64_t value) {
    if (!impl_ || !impl_->valid) return std::unexpected(HalError::NotInitialized);
    hv_sys_reg_t sys_reg;
    switch (index) {
        case 0x6000: sys_reg = HV_SYS_REG_SCTLR_EL1; break;
        case 0x6001: sys_reg = HV_SYS_REG_TTBR0_EL1; break;
        case 0x6002: sys_reg = HV_SYS_REG_TTBR1_EL1; break;
        case 0x6003: sys_reg = HV_SYS_REG_TCR_EL1; break;
        case 0x6004: sys_reg = HV_SYS_REG_MAIR_EL1; break;
        case 0x6005: sys_reg = HV_SYS_REG_VBAR_EL1; break;
        default: return std::unexpected(HalError::NotSupported);
    }
    if (hv_vcpu_set_sys_reg(impl_->handle, sys_reg, value) != HV_SUCCESS)
        return std::unexpected(HalError::InternalError);
    return {};
}

// ============================================================================
// HvfArm64MemoryManager
// ============================================================================

HalResult<void> HvfArm64MemoryManager::map_region(const MemoryRegion& region) {
    hv_memory_flags_t flags = HV_MEMORY_READ;
    if (!region.readonly) flags |= HV_MEMORY_WRITE | HV_MEMORY_EXEC;
    hv_return_t ret = hv_vm_map(
        reinterpret_cast<void*>(region.userspace_addr),
        region.guest_phys_addr, region.size, flags);
    if (ret != HV_SUCCESS) return std::unexpected(HalError::MemoryMappingFailed);
    for (auto& r : regions_) {
        if (r.slot == region.slot) { r = region; return {}; }
    }
    regions_.push_back(region);
    return {};
}

HalResult<void> HvfArm64MemoryManager::unmap_region(uint32_t slot) {
    auto it = std::find_if(regions_.begin(), regions_.end(),
        [slot](const MemoryRegion& r) { return r.slot == slot; });
    if (it == regions_.end()) return std::unexpected(HalError::InvalidParameter);
    hv_vm_unmap(it->guest_phys_addr, it->size);
    regions_.erase(it);
    return {};
}

HalResult<HVA> HvfArm64MemoryManager::gpa_to_hva(GPA gpa) const {
    for (const auto& r : regions_) {
        if (gpa >= r.guest_phys_addr && gpa < r.guest_phys_addr + r.size)
            return r.userspace_addr + (gpa - r.guest_phys_addr);
    }
    return std::unexpected(HalError::InvalidParameter);
}

std::vector<MemoryRegion> HvfArm64MemoryManager::get_regions() const {
    return regions_;
}

// ============================================================================
// HvfArm64Hypervisor
// ============================================================================

HvfArm64Hypervisor::HvfArm64Hypervisor() = default;

HvfArm64Hypervisor::~HvfArm64Hypervisor() {
    if (vm_created_) hv_vm_destroy();
}

bool HvfArm64Hypervisor::is_available() const {
    hv_return_t ret = hv_vm_create(nullptr);
    if (ret == HV_SUCCESS) { hv_vm_destroy(); return true; }
    return false;
}

HalResult<void> HvfArm64Hypervisor::initialize() { return {}; }

HalResult<void> HvfArm64Hypervisor::create_vm(const VmConfig& config) {
    config_ = config;
    hv_return_t ret = hv_vm_create(nullptr);
    if (ret != HV_SUCCESS) return std::unexpected(HalError::InternalError);
    vm_created_ = true;
    mem_mgr_ = std::make_unique<HvfArm64MemoryManager>();
    return {};
}

HalResult<std::unique_ptr<IVcpu>> HvfArm64Hypervisor::create_vcpu(VcpuId id) {
    if (!vm_created_) return std::unexpected(HalError::NotInitialized);
    auto vcpu = std::make_unique<HvfArm64VcpuAdapter>(id);
    if (!vcpu->is_valid()) return std::unexpected(HalError::InternalError);
    return vcpu;
}

IMemoryManager& HvfArm64Hypervisor::memory_manager() { return *mem_mgr_; }
HalResult<void> HvfArm64Hypervisor::create_irqchip() { return {}; }
HalResult<void> HvfArm64Hypervisor::set_irq_line(uint32_t, bool) {
    return std::unexpected(HalError::NotSupported);
}
int HvfArm64Hypervisor::api_version() const { return 1; }

} // namespace rex::hal

#endif // __APPLE__ && __aarch64__
