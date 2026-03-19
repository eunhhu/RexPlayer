#include "hvf_hypervisor.h"

#ifdef __APPLE__

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <sys/mman.h>
#include <cstring>

namespace rex::hal {

// ============================================================================
// HvfVcpu
// ============================================================================

HvfVcpu::HvfVcpu(VcpuId id) : id_(id) {
    hv_vcpuid_t vcpu;
    if (hv_vcpu_create(&vcpu, HV_VCPU_DEFAULT) == HV_SUCCESS) {
        vcpu_handle_ = vcpu;
        initialized_ = true;
    }
}

HvfVcpu::~HvfVcpu() {
    if (initialized_) {
        hv_vcpu_destroy(static_cast<hv_vcpuid_t>(vcpu_handle_));
    }
}

HalResult<VcpuExit> HvfVcpu::run() {
    if (!initialized_) {
        return std::unexpected(HalError::NotInitialized);
    }

    auto vcpu = static_cast<hv_vcpuid_t>(vcpu_handle_);
    hv_return_t ret = hv_vcpu_run(vcpu);
    if (ret != HV_SUCCESS) {
        return std::unexpected(HalError::VcpuRunFailed);
    }

    VcpuExit exit{};
    uint64_t exit_reason = 0;
    hv_vmx_vcpu_read_vmcs(vcpu, VMCS_RO_EXIT_REASON, &exit_reason);

    switch (exit_reason) {
        case VMX_REASON_IO: {
            exit.reason = VcpuExit::Reason::IoAccess;
            uint64_t qual = 0;
            hv_vmx_vcpu_read_vmcs(vcpu, VMCS_RO_EXIT_QUALIFIC, &qual);
            exit.io.port = static_cast<uint16_t>((qual >> 16) & 0xFFFF);
            exit.io.size = static_cast<uint8_t>((qual & 7) + 1);
            exit.io.is_write = !(qual & (1 << 3));
            if (exit.io.is_write) {
                uint64_t rax = 0;
                hv_vcpu_read_register(vcpu, HV_X86_RAX, &rax);
                exit.io.data = static_cast<uint32_t>(rax);
            }
            break;
        }
        case VMX_REASON_EPT_VIOLATION: {
            exit.reason = VcpuExit::Reason::MmioAccess;
            uint64_t gpa = 0;
            hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_PHYSICAL_ADDRESS, &gpa);
            uint64_t qual = 0;
            hv_vmx_vcpu_read_vmcs(vcpu, VMCS_RO_EXIT_QUALIFIC, &qual);
            exit.mmio.address = gpa;
            exit.mmio.is_write = (qual & 2) != 0;
            exit.mmio.size = 4; // Default, actual size needs instruction decode
            break;
        }
        case VMX_REASON_HLT:
            exit.reason = VcpuExit::Reason::Hlt;
            break;
        case VMX_REASON_TRIPLE_FAULT:
            exit.reason = VcpuExit::Reason::Shutdown;
            break;
        case VMX_REASON_IRQ:
            exit.reason = VcpuExit::Reason::IrqWindowOpen;
            break;
        default:
            exit.reason = VcpuExit::Reason::Unknown;
            break;
    }

    return exit;
}

HalResult<X86Regs> HvfVcpu::get_regs() const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    auto vcpu = static_cast<hv_vcpuid_t>(vcpu_handle_);

    X86Regs regs{};
    hv_vcpu_read_register(vcpu, HV_X86_RAX, &regs.rax);
    hv_vcpu_read_register(vcpu, HV_X86_RBX, &regs.rbx);
    hv_vcpu_read_register(vcpu, HV_X86_RCX, &regs.rcx);
    hv_vcpu_read_register(vcpu, HV_X86_RDX, &regs.rdx);
    hv_vcpu_read_register(vcpu, HV_X86_RSI, &regs.rsi);
    hv_vcpu_read_register(vcpu, HV_X86_RDI, &regs.rdi);
    hv_vcpu_read_register(vcpu, HV_X86_RBP, &regs.rbp);
    hv_vcpu_read_register(vcpu, HV_X86_RSP, &regs.rsp);
    hv_vcpu_read_register(vcpu, HV_X86_R8,  &regs.r8);
    hv_vcpu_read_register(vcpu, HV_X86_R9,  &regs.r9);
    hv_vcpu_read_register(vcpu, HV_X86_R10, &regs.r10);
    hv_vcpu_read_register(vcpu, HV_X86_R11, &regs.r11);
    hv_vcpu_read_register(vcpu, HV_X86_R12, &regs.r12);
    hv_vcpu_read_register(vcpu, HV_X86_R13, &regs.r13);
    hv_vcpu_read_register(vcpu, HV_X86_R14, &regs.r14);
    hv_vcpu_read_register(vcpu, HV_X86_R15, &regs.r15);
    hv_vcpu_read_register(vcpu, HV_X86_RIP, &regs.rip);
    hv_vcpu_read_register(vcpu, HV_X86_RFLAGS, &regs.rflags);
    return regs;
}

HalResult<void> HvfVcpu::set_regs(const X86Regs& regs) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    auto vcpu = static_cast<hv_vcpuid_t>(vcpu_handle_);

    hv_vcpu_write_register(vcpu, HV_X86_RAX, regs.rax);
    hv_vcpu_write_register(vcpu, HV_X86_RBX, regs.rbx);
    hv_vcpu_write_register(vcpu, HV_X86_RCX, regs.rcx);
    hv_vcpu_write_register(vcpu, HV_X86_RDX, regs.rdx);
    hv_vcpu_write_register(vcpu, HV_X86_RSI, regs.rsi);
    hv_vcpu_write_register(vcpu, HV_X86_RDI, regs.rdi);
    hv_vcpu_write_register(vcpu, HV_X86_RBP, regs.rbp);
    hv_vcpu_write_register(vcpu, HV_X86_RSP, regs.rsp);
    hv_vcpu_write_register(vcpu, HV_X86_R8,  regs.r8);
    hv_vcpu_write_register(vcpu, HV_X86_R9,  regs.r9);
    hv_vcpu_write_register(vcpu, HV_X86_R10, regs.r10);
    hv_vcpu_write_register(vcpu, HV_X86_R11, regs.r11);
    hv_vcpu_write_register(vcpu, HV_X86_R12, regs.r12);
    hv_vcpu_write_register(vcpu, HV_X86_R13, regs.r13);
    hv_vcpu_write_register(vcpu, HV_X86_R14, regs.r14);
    hv_vcpu_write_register(vcpu, HV_X86_R15, regs.r15);
    hv_vcpu_write_register(vcpu, HV_X86_RIP, regs.rip);
    hv_vcpu_write_register(vcpu, HV_X86_RFLAGS, regs.rflags);
    return {};
}

HalResult<X86Sregs> HvfVcpu::get_sregs() const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    auto vcpu = static_cast<hv_vcpuid_t>(vcpu_handle_);

    X86Sregs sregs{};

    auto read_seg = [vcpu](hv_x86_reg_t base_reg, hv_x86_reg_t limit_reg,
                           hv_x86_reg_t sel_reg, hv_x86_reg_t ar_reg) -> X86Segment {
        X86Segment seg{};
        uint64_t val = 0;
        hv_vcpu_read_register(vcpu, base_reg, &val);  seg.base = val;
        hv_vcpu_read_register(vcpu, limit_reg, &val);  seg.limit = static_cast<uint32_t>(val);
        hv_vcpu_read_register(vcpu, sel_reg, &val);    seg.selector = static_cast<uint16_t>(val);
        hv_vcpu_read_register(vcpu, ar_reg, &val);
        seg.type = val & 0xF;
        seg.s = (val >> 4) & 1;
        seg.dpl = (val >> 5) & 3;
        seg.present = (val >> 7) & 1;
        seg.avl = (val >> 12) & 1;
        seg.l = (val >> 13) & 1;
        seg.db = (val >> 14) & 1;
        seg.g = (val >> 15) & 1;
        return seg;
    };

    sregs.cs = read_seg(HV_X86_CS_BASE, HV_X86_CS_LIMIT, HV_X86_CS, HV_X86_CS_AR);
    sregs.ds = read_seg(HV_X86_DS_BASE, HV_X86_DS_LIMIT, HV_X86_DS, HV_X86_DS_AR);
    sregs.es = read_seg(HV_X86_ES_BASE, HV_X86_ES_LIMIT, HV_X86_ES, HV_X86_ES_AR);
    sregs.fs = read_seg(HV_X86_FS_BASE, HV_X86_FS_LIMIT, HV_X86_FS, HV_X86_FS_AR);
    sregs.gs = read_seg(HV_X86_GS_BASE, HV_X86_GS_LIMIT, HV_X86_GS, HV_X86_GS_AR);
    sregs.ss = read_seg(HV_X86_SS_BASE, HV_X86_SS_LIMIT, HV_X86_SS, HV_X86_SS_AR);
    sregs.tr = read_seg(HV_X86_TR_BASE, HV_X86_TR_LIMIT, HV_X86_TR, HV_X86_TR_AR);
    sregs.ldt = read_seg(HV_X86_LDTR_BASE, HV_X86_LDTR_LIMIT, HV_X86_LDTR, HV_X86_LDTR_AR);

    uint64_t val = 0;
    hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_GDTR_BASE, &val);  sregs.gdt.base = val;
    hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_GDTR_LIMIT, &val); sregs.gdt.limit = static_cast<uint16_t>(val);
    hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_IDTR_BASE, &val);  sregs.idt.base = val;
    hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_IDTR_LIMIT, &val); sregs.idt.limit = static_cast<uint16_t>(val);

    hv_vcpu_read_register(vcpu, HV_X86_CR0, &val); sregs.cr0 = val;
    hv_vcpu_read_register(vcpu, HV_X86_CR2, &val); sregs.cr2 = val;
    hv_vcpu_read_register(vcpu, HV_X86_CR3, &val); sregs.cr3 = val;
    hv_vcpu_read_register(vcpu, HV_X86_CR4, &val); sregs.cr4 = val;
    hv_vmx_vcpu_read_vmcs(vcpu, VMCS_GUEST_IA32_EFER, &val); sregs.efer = val;

    return sregs;
}

HalResult<void> HvfVcpu::set_sregs(const X86Sregs& sregs) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    auto vcpu = static_cast<hv_vcpuid_t>(vcpu_handle_);

    auto write_seg = [vcpu](const X86Segment& seg, hv_x86_reg_t base_reg,
                            hv_x86_reg_t limit_reg, hv_x86_reg_t sel_reg,
                            hv_x86_reg_t ar_reg) {
        hv_vcpu_write_register(vcpu, base_reg, seg.base);
        hv_vcpu_write_register(vcpu, limit_reg, seg.limit);
        hv_vcpu_write_register(vcpu, sel_reg, seg.selector);
        uint64_t ar = seg.type | (static_cast<uint64_t>(seg.s) << 4) |
                      (static_cast<uint64_t>(seg.dpl) << 5) |
                      (static_cast<uint64_t>(seg.present) << 7) |
                      (static_cast<uint64_t>(seg.avl) << 12) |
                      (static_cast<uint64_t>(seg.l) << 13) |
                      (static_cast<uint64_t>(seg.db) << 14) |
                      (static_cast<uint64_t>(seg.g) << 15);
        hv_vcpu_write_register(vcpu, ar_reg, ar);
    };

    write_seg(sregs.cs, HV_X86_CS_BASE, HV_X86_CS_LIMIT, HV_X86_CS, HV_X86_CS_AR);
    write_seg(sregs.ds, HV_X86_DS_BASE, HV_X86_DS_LIMIT, HV_X86_DS, HV_X86_DS_AR);
    write_seg(sregs.es, HV_X86_ES_BASE, HV_X86_ES_LIMIT, HV_X86_ES, HV_X86_ES_AR);
    write_seg(sregs.fs, HV_X86_FS_BASE, HV_X86_FS_LIMIT, HV_X86_FS, HV_X86_FS_AR);
    write_seg(sregs.gs, HV_X86_GS_BASE, HV_X86_GS_LIMIT, HV_X86_GS, HV_X86_GS_AR);
    write_seg(sregs.ss, HV_X86_SS_BASE, HV_X86_SS_LIMIT, HV_X86_SS, HV_X86_SS_AR);
    write_seg(sregs.tr, HV_X86_TR_BASE, HV_X86_TR_LIMIT, HV_X86_TR, HV_X86_TR_AR);
    write_seg(sregs.ldt, HV_X86_LDTR_BASE, HV_X86_LDTR_LIMIT, HV_X86_LDTR, HV_X86_LDTR_AR);

    hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_GDTR_BASE, sregs.gdt.base);
    hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_GDTR_LIMIT, sregs.gdt.limit);
    hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_IDTR_BASE, sregs.idt.base);
    hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_IDTR_LIMIT, sregs.idt.limit);

    hv_vcpu_write_register(vcpu, HV_X86_CR0, sregs.cr0);
    hv_vcpu_write_register(vcpu, HV_X86_CR2, sregs.cr2);
    hv_vcpu_write_register(vcpu, HV_X86_CR3, sregs.cr3);
    hv_vcpu_write_register(vcpu, HV_X86_CR4, sregs.cr4);
    hv_vmx_vcpu_write_vmcs(vcpu, VMCS_GUEST_IA32_EFER, sregs.efer);

    return {};
}

HalResult<void> HvfVcpu::inject_interrupt(uint32_t irq) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    auto vcpu = static_cast<hv_vcpuid_t>(vcpu_handle_);

    // VMX interrupt injection via VMCS entry interrupt info field
    uint64_t info = irq | (0 << 8) /* external interrupt */ | (1 << 31) /* valid */;
    hv_vmx_vcpu_write_vmcs(vcpu, VMCS_CTRL_VMENTRY_IRQ_INFO, info);
    return {};
}

HalResult<uint64_t> HvfVcpu::get_msr(uint32_t index) const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    uint64_t value = 0;
    if (hv_vcpu_read_msr(static_cast<hv_vcpuid_t>(vcpu_handle_), index, &value) != HV_SUCCESS) {
        return std::unexpected(HalError::NotSupported);
    }
    return value;
}

HalResult<void> HvfVcpu::set_msr(uint32_t index, uint64_t value) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    if (hv_vcpu_write_msr(static_cast<hv_vcpuid_t>(vcpu_handle_), index, value) != HV_SUCCESS) {
        return std::unexpected(HalError::NotSupported);
    }
    return {};
}

// ============================================================================
// HvfMemoryManager
// ============================================================================

HalResult<void> HvfMemoryManager::map_region(const MemoryRegion& region) {
    hv_memory_flags_t flags = HV_MEMORY_READ;
    if (!region.readonly) {
        flags |= HV_MEMORY_WRITE | HV_MEMORY_EXEC;
    }

    hv_return_t ret = hv_vm_map(
        reinterpret_cast<void*>(region.userspace_addr),
        region.guest_phys_addr,
        region.size,
        flags
    );

    if (ret != HV_SUCCESS) {
        return std::unexpected(HalError::MemoryMappingFailed);
    }

    for (auto& r : regions_) {
        if (r.slot == region.slot) {
            r = region;
            return {};
        }
    }
    regions_.push_back(region);
    return {};
}

HalResult<void> HvfMemoryManager::unmap_region(uint32_t slot) {
    auto it = std::find_if(regions_.begin(), regions_.end(),
        [slot](const MemoryRegion& r) { return r.slot == slot; });

    if (it == regions_.end()) {
        return std::unexpected(HalError::InvalidParameter);
    }

    hv_return_t ret = hv_vm_unmap(it->guest_phys_addr, it->size);
    if (ret != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    regions_.erase(it);
    return {};
}

HalResult<HVA> HvfMemoryManager::gpa_to_hva(GPA gpa) const {
    for (const auto& r : regions_) {
        if (gpa >= r.guest_phys_addr && gpa < r.guest_phys_addr + r.size) {
            return r.userspace_addr + (gpa - r.guest_phys_addr);
        }
    }
    return std::unexpected(HalError::InvalidParameter);
}

std::vector<MemoryRegion> HvfMemoryManager::get_regions() const {
    return regions_;
}

// ============================================================================
// HvfHypervisor
// ============================================================================

HvfHypervisor::HvfHypervisor() = default;

HvfHypervisor::~HvfHypervisor() {
    if (vm_created_) {
        hv_vm_destroy();
    }
}

bool HvfHypervisor::is_available() const {
    // Try creating and immediately destroying a VM to test availability
    hv_return_t ret = hv_vm_create(HV_VM_DEFAULT);
    if (ret == HV_SUCCESS) {
        hv_vm_destroy();
        return true;
    }
    return false;
}

HalResult<void> HvfHypervisor::initialize() {
    // HVF doesn't require explicit initialization beyond vm_create
    return {};
}

HalResult<void> HvfHypervisor::create_vm(const VmConfig& config) {
    config_ = config;

    hv_return_t ret = hv_vm_create(HV_VM_DEFAULT);
    if (ret != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    vm_created_ = true;
    mem_mgr_ = std::make_unique<HvfMemoryManager>();
    return {};
}

HalResult<std::unique_ptr<IVcpu>> HvfHypervisor::create_vcpu(VcpuId id) {
    if (!vm_created_) {
        return std::unexpected(HalError::NotInitialized);
    }
    return std::make_unique<HvfVcpu>(id);
}

IMemoryManager& HvfHypervisor::memory_manager() {
    return *mem_mgr_;
}

HalResult<void> HvfHypervisor::create_irqchip() {
    // HVF on x86 doesn't provide in-kernel APIC; must be emulated in userspace
    // For now, return OK and handle interrupt routing in VMM layer
    return {};
}

HalResult<void> HvfHypervisor::set_irq_line(uint32_t /*irq*/, bool /*level*/) {
    // Userspace APIC emulation needed
    return std::unexpected(HalError::NotSupported);
}

int HvfHypervisor::api_version() const {
    return 1; // HVF doesn't have a version API
}

} // namespace rex::hal

#endif // __APPLE__
