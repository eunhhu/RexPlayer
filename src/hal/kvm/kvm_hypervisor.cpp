#include "kvm_hypervisor.h"

#ifdef __linux__

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <cstring>

namespace rex::hal {

// ============================================================================
// KvmVcpu
// ============================================================================

KvmVcpu::KvmVcpu(int vm_fd, VcpuId id) : id_(id) {
    vcpu_fd_ = ioctl(vm_fd, KVM_CREATE_VCPU, id);
    if (vcpu_fd_ < 0) return;

    // Get the size of the kvm_run mmap region
    int kvm_fd = open("/dev/kvm", O_RDONLY);
    if (kvm_fd >= 0) {
        kvm_run_size_ = static_cast<size_t>(ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0));
        close(kvm_fd);
    }

    if (kvm_run_size_ > 0) {
        kvm_run_ = mmap(nullptr, kvm_run_size_, PROT_READ | PROT_WRITE,
                        MAP_SHARED, vcpu_fd_, 0);
        if (kvm_run_ == MAP_FAILED) {
            kvm_run_ = nullptr;
        }
    }
}

KvmVcpu::~KvmVcpu() {
    if (kvm_run_ && kvm_run_size_ > 0) {
        munmap(kvm_run_, kvm_run_size_);
    }
    if (vcpu_fd_ >= 0) {
        close(vcpu_fd_);
    }
}

HalResult<VcpuExit> KvmVcpu::run() {
    if (vcpu_fd_ < 0 || !kvm_run_) {
        return std::unexpected(HalError::NotInitialized);
    }

    int ret = ioctl(vcpu_fd_, KVM_RUN, 0);
    if (ret < 0) {
        return std::unexpected(HalError::VcpuRunFailed);
    }

    auto* run = static_cast<struct kvm_run*>(kvm_run_);
    VcpuExit exit{};

    switch (run->exit_reason) {
        case KVM_EXIT_IO: {
            exit.reason = VcpuExit::Reason::IoAccess;
            exit.io.port = run->io.port;
            exit.io.size = run->io.size;
            exit.io.is_write = (run->io.direction == KVM_EXIT_IO_OUT);
            if (exit.io.is_write) {
                auto* data_ptr = reinterpret_cast<uint8_t*>(run) + run->io.data_offset;
                std::memcpy(&exit.io.data, data_ptr, run->io.size);
            }
            break;
        }
        case KVM_EXIT_MMIO: {
            exit.reason = VcpuExit::Reason::MmioAccess;
            exit.mmio.address = run->mmio.phys_addr;
            exit.mmio.size = run->mmio.len;
            exit.mmio.is_write = run->mmio.is_write;
            if (exit.mmio.is_write) {
                std::memcpy(&exit.mmio.data, run->mmio.data, run->mmio.len);
            }
            break;
        }
        case KVM_EXIT_HLT:
            exit.reason = VcpuExit::Reason::Hlt;
            break;
        case KVM_EXIT_SHUTDOWN:
            exit.reason = VcpuExit::Reason::Shutdown;
            break;
        case KVM_EXIT_IRQ_WINDOW_OPEN:
            exit.reason = VcpuExit::Reason::IrqWindowOpen;
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            exit.reason = VcpuExit::Reason::InternalError;
            break;
        default:
            exit.reason = VcpuExit::Reason::Unknown;
            break;
    }

    return exit;
}

HalResult<X86Regs> KvmVcpu::get_regs() const {
    struct kvm_regs kregs{};
    if (ioctl(vcpu_fd_, KVM_GET_REGS, &kregs) < 0) {
        return std::unexpected(HalError::InternalError);
    }

    X86Regs regs{};
    regs.rax = kregs.rax; regs.rbx = kregs.rbx;
    regs.rcx = kregs.rcx; regs.rdx = kregs.rdx;
    regs.rsi = kregs.rsi; regs.rdi = kregs.rdi;
    regs.rbp = kregs.rbp; regs.rsp = kregs.rsp;
    regs.r8  = kregs.r8;  regs.r9  = kregs.r9;
    regs.r10 = kregs.r10; regs.r11 = kregs.r11;
    regs.r12 = kregs.r12; regs.r13 = kregs.r13;
    regs.r14 = kregs.r14; regs.r15 = kregs.r15;
    regs.rip = kregs.rip; regs.rflags = kregs.rflags;
    return regs;
}

HalResult<void> KvmVcpu::set_regs(const X86Regs& regs) {
    struct kvm_regs kregs{};
    kregs.rax = regs.rax; kregs.rbx = regs.rbx;
    kregs.rcx = regs.rcx; kregs.rdx = regs.rdx;
    kregs.rsi = regs.rsi; kregs.rdi = regs.rdi;
    kregs.rbp = regs.rbp; kregs.rsp = regs.rsp;
    kregs.r8  = regs.r8;  kregs.r9  = regs.r9;
    kregs.r10 = regs.r10; kregs.r11 = regs.r11;
    kregs.r12 = regs.r12; kregs.r13 = regs.r13;
    kregs.r14 = regs.r14; kregs.r15 = regs.r15;
    kregs.rip = regs.rip; kregs.rflags = regs.rflags;

    if (ioctl(vcpu_fd_, KVM_SET_REGS, &kregs) < 0) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<X86Sregs> KvmVcpu::get_sregs() const {
    struct kvm_sregs ksregs{};
    if (ioctl(vcpu_fd_, KVM_GET_SREGS, &ksregs) < 0) {
        return std::unexpected(HalError::InternalError);
    }

    auto convert_seg = [](const struct kvm_segment& ks) -> X86Segment {
        return {
            .base = ks.base, .limit = ks.limit, .selector = ks.selector,
            .type = ks.type, .present = ks.present, .dpl = ks.dpl,
            .db = ks.db, .s = ks.s, .l = ks.l, .g = ks.g, .avl = ks.avl
        };
    };

    X86Sregs sregs{};
    sregs.cs = convert_seg(ksregs.cs);
    sregs.ds = convert_seg(ksregs.ds);
    sregs.es = convert_seg(ksregs.es);
    sregs.fs = convert_seg(ksregs.fs);
    sregs.gs = convert_seg(ksregs.gs);
    sregs.ss = convert_seg(ksregs.ss);
    sregs.tr = convert_seg(ksregs.tr);
    sregs.ldt = convert_seg(ksregs.ldt);
    sregs.gdt = { .base = ksregs.gdt.base, .limit = ksregs.gdt.limit };
    sregs.idt = { .base = ksregs.idt.base, .limit = ksregs.idt.limit };
    sregs.cr0 = ksregs.cr0; sregs.cr2 = ksregs.cr2;
    sregs.cr3 = ksregs.cr3; sregs.cr4 = ksregs.cr4;
    sregs.efer = ksregs.efer;
    return sregs;
}

HalResult<void> KvmVcpu::set_sregs(const X86Sregs& sregs) {
    struct kvm_sregs ksregs{};

    auto convert_seg = [](const X86Segment& seg) -> struct kvm_segment {
        struct kvm_segment ks{};
        ks.base = seg.base; ks.limit = seg.limit; ks.selector = seg.selector;
        ks.type = seg.type; ks.present = seg.present; ks.dpl = seg.dpl;
        ks.db = seg.db; ks.s = seg.s; ks.l = seg.l; ks.g = seg.g; ks.avl = seg.avl;
        return ks;
    };

    ksregs.cs = convert_seg(sregs.cs);
    ksregs.ds = convert_seg(sregs.ds);
    ksregs.es = convert_seg(sregs.es);
    ksregs.fs = convert_seg(sregs.fs);
    ksregs.gs = convert_seg(sregs.gs);
    ksregs.ss = convert_seg(sregs.ss);
    ksregs.tr = convert_seg(sregs.tr);
    ksregs.ldt = convert_seg(sregs.ldt);
    ksregs.gdt = { .base = sregs.gdt.base, .limit = sregs.gdt.limit };
    ksregs.idt = { .base = sregs.idt.base, .limit = sregs.idt.limit };
    ksregs.cr0 = sregs.cr0; ksregs.cr2 = sregs.cr2;
    ksregs.cr3 = sregs.cr3; ksregs.cr4 = sregs.cr4;
    ksregs.efer = sregs.efer;

    if (ioctl(vcpu_fd_, KVM_SET_SREGS, &ksregs) < 0) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<void> KvmVcpu::inject_interrupt(uint32_t irq) {
    struct kvm_interrupt intr{};
    intr.irq = irq;
    if (ioctl(vcpu_fd_, KVM_INTERRUPT, &intr) < 0) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<uint64_t> KvmVcpu::get_msr(uint32_t index) const {
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entry;
    } msrs{};
    msrs.header.nmsrs = 1;
    msrs.entry.index = index;

    if (ioctl(vcpu_fd_, KVM_GET_MSRS, &msrs) < 0) {
        return std::unexpected(HalError::InternalError);
    }
    return msrs.entry.data;
}

HalResult<void> KvmVcpu::set_msr(uint32_t index, uint64_t value) {
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entry;
    } msrs{};
    msrs.header.nmsrs = 1;
    msrs.entry.index = index;
    msrs.entry.data = value;

    if (ioctl(vcpu_fd_, KVM_SET_MSRS, &msrs) < 0) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

// ============================================================================
// KvmMemoryManager
// ============================================================================

KvmMemoryManager::KvmMemoryManager(int vm_fd) : vm_fd_(vm_fd) {}

HalResult<void> KvmMemoryManager::map_region(const MemoryRegion& region) {
    struct kvm_userspace_memory_region kmem{};
    kmem.slot = region.slot;
    kmem.guest_phys_addr = region.guest_phys_addr;
    kmem.memory_size = region.size;
    kmem.userspace_addr = region.userspace_addr;
    kmem.flags = region.readonly ? KVM_MEM_READONLY : 0;

    if (ioctl(vm_fd_, KVM_SET_USER_MEMORY_REGION, &kmem) < 0) {
        return std::unexpected(HalError::MemoryMappingFailed);
    }

    // Track the region
    for (auto& r : regions_) {
        if (r.slot == region.slot) {
            r = region;
            return {};
        }
    }
    regions_.push_back(region);
    return {};
}

HalResult<void> KvmMemoryManager::unmap_region(uint32_t slot) {
    struct kvm_userspace_memory_region kmem{};
    kmem.slot = slot;
    kmem.memory_size = 0; // size=0 removes the region

    if (ioctl(vm_fd_, KVM_SET_USER_MEMORY_REGION, &kmem) < 0) {
        return std::unexpected(HalError::InternalError);
    }

    std::erase_if(regions_, [slot](const MemoryRegion& r) {
        return r.slot == slot;
    });
    return {};
}

HalResult<HVA> KvmMemoryManager::gpa_to_hva(GPA gpa) const {
    for (const auto& r : regions_) {
        if (gpa >= r.guest_phys_addr && gpa < r.guest_phys_addr + r.size) {
            return r.userspace_addr + (gpa - r.guest_phys_addr);
        }
    }
    return std::unexpected(HalError::InvalidParameter);
}

std::vector<MemoryRegion> KvmMemoryManager::get_regions() const {
    return regions_;
}

// ============================================================================
// KvmHypervisor
// ============================================================================

KvmHypervisor::KvmHypervisor() = default;

KvmHypervisor::~KvmHypervisor() {
    if (vm_fd_ >= 0) close(vm_fd_);
    if (kvm_fd_ >= 0) close(kvm_fd_);
}

bool KvmHypervisor::is_available() const {
    int fd = open("/dev/kvm", O_RDWR);
    if (fd < 0) return false;
    close(fd);
    return true;
}

HalResult<void> KvmHypervisor::initialize() {
    kvm_fd_ = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd_ < 0) {
        return std::unexpected(HalError::PermissionDenied);
    }

    int version = ioctl(kvm_fd_, KVM_GET_API_VERSION, 0);
    if (version != 12) {
        close(kvm_fd_);
        kvm_fd_ = -1;
        return std::unexpected(HalError::NotSupported);
    }

    return {};
}

HalResult<void> KvmHypervisor::create_vm(const VmConfig& config) {
    if (kvm_fd_ < 0) {
        return std::unexpected(HalError::NotInitialized);
    }

    config_ = config;
    vm_fd_ = ioctl(kvm_fd_, KVM_CREATE_VM, 0);
    if (vm_fd_ < 0) {
        return std::unexpected(HalError::InternalError);
    }

    mem_mgr_ = std::make_unique<KvmMemoryManager>(vm_fd_);

    if (config.enable_irqchip) {
        auto result = create_irqchip();
        if (!result) return result;
    }

    return {};
}

HalResult<std::unique_ptr<IVcpu>> KvmHypervisor::create_vcpu(VcpuId id) {
    if (vm_fd_ < 0) {
        return std::unexpected(HalError::NotInitialized);
    }

    auto vcpu = std::make_unique<KvmVcpu>(vm_fd_, id);
    return vcpu;
}

IMemoryManager& KvmHypervisor::memory_manager() {
    return *mem_mgr_;
}

HalResult<void> KvmHypervisor::create_irqchip() {
    if (ioctl(vm_fd_, KVM_CREATE_IRQCHIP, 0) < 0) {
        return std::unexpected(HalError::InternalError);
    }

    // Create PIT (i8254 timer)
    struct kvm_pit_config pit_config{};
    pit_config.flags = KVM_PIT_SPEAKER_DUMMY;
    if (ioctl(vm_fd_, KVM_CREATE_PIT2, &pit_config) < 0) {
        return std::unexpected(HalError::InternalError);
    }

    return {};
}

HalResult<void> KvmHypervisor::set_irq_line(uint32_t irq, bool level) {
    struct kvm_irq_level irq_level{};
    irq_level.irq = irq;
    irq_level.level = level ? 1 : 0;

    if (ioctl(vm_fd_, KVM_IRQ_LINE, &irq_level) < 0) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

int KvmHypervisor::api_version() const {
    if (kvm_fd_ < 0) return -1;
    return ioctl(kvm_fd_, KVM_GET_API_VERSION, 0);
}

} // namespace rex::hal

#endif // __linux__
