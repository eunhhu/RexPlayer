#if defined(__APPLE__) && defined(__aarch64__)

#include "hvf_arm64_types.h"
#include "rex/hal/hypervisor.h"
#include "rex/hal/memory.h"

#include <Hypervisor/hv.h>
#include <sys/mman.h>
#include <cstring>
#include <vector>
#include <memory>
#include <mutex>

namespace rex::hal {

// ============================================================================
// HvfArm64Vcpu — ARM64 vCPU backed by Apple Hypervisor.framework
// ============================================================================

class HvfArm64Vcpu {
public:
    explicit HvfArm64Vcpu(VcpuId id);
    ~HvfArm64Vcpu();

    // Non-copyable, non-movable
    HvfArm64Vcpu(const HvfArm64Vcpu&) = delete;
    HvfArm64Vcpu& operator=(const HvfArm64Vcpu&) = delete;

    /// Run the vCPU until it exits
    HalResult<Arm64VcpuExit> run();

    /// Get the vCPU identifier
    VcpuId id() const { return id_; }

    /// Whether this vCPU was successfully created
    bool is_valid() const { return initialized_; }

    // -- General-purpose registers --

    HalResult<Arm64Regs> get_regs() const;
    HalResult<void> set_regs(const Arm64Regs& regs);

    // -- System registers --

    HalResult<Arm64SysRegs> get_sys_regs() const;
    HalResult<void> set_sys_regs(const Arm64SysRegs& sregs);

    // -- SIMD/FP registers --

    HalResult<Arm64FpRegs> get_fp_regs() const;
    HalResult<void> set_fp_regs(const Arm64FpRegs& regs);

    // -- Individual register access --

    HalResult<uint64_t> get_reg(hv_reg_t reg) const;
    HalResult<void> set_reg(hv_reg_t reg, uint64_t value);

    HalResult<uint64_t> get_sys_reg(hv_sys_reg_t reg) const;
    HalResult<void> set_sys_reg(hv_sys_reg_t reg, uint64_t value);

    // -- Interrupt injection --

    HalResult<void> set_pending_interrupt(hv_interrupt_type_t type, bool pending);

    // -- Virtual timer --

    HalResult<void> set_vtimer_mask(bool masked);
    HalResult<hv_vcpu_exit_t*> get_exit_info() const;

private:
    VcpuId id_;
    hv_vcpu_t vcpu_handle_ = 0;
    hv_vcpu_exit_t* exit_info_ = nullptr;
    bool initialized_ = false;
};

// ============================================================================
// HvfArm64Vcpu implementation
// ============================================================================

HvfArm64Vcpu::HvfArm64Vcpu(VcpuId id) : id_(id) {
    hv_vcpu_config_t config = hv_vcpu_config_create();
    hv_return_t ret = hv_vcpu_create(&vcpu_handle_, &exit_info_, config);
    if (config) {
        // config is a reference-counted object; release after vcpu creation
        os_release(config);
    }
    if (ret == HV_SUCCESS) {
        initialized_ = true;
    }
}

HvfArm64Vcpu::~HvfArm64Vcpu() {
    if (initialized_) {
        hv_vcpu_destroy(vcpu_handle_);
    }
}

HalResult<Arm64VcpuExit> HvfArm64Vcpu::run() {
    if (!initialized_) {
        return std::unexpected(HalError::NotInitialized);
    }

    hv_return_t ret = hv_vcpu_run(vcpu_handle_);
    if (ret != HV_SUCCESS) {
        return std::unexpected(HalError::VcpuRunFailed);
    }

    Arm64VcpuExit exit{};

    switch (exit_info_->reason) {
        case HV_EXIT_REASON_EXCEPTION: {
            exit.reason = Arm64VcpuExit::Reason::Exception;
            exit.exception.syndrome = exit_info_->exception.syndrome;
            exit.exception.va = exit_info_->exception.virtual_address;
            exit.exception.pa = exit_info_->exception.physical_address;

            // Decode based on exception class
            auto ec = exit.exception_class();
            switch (static_cast<uint8_t>(ec)) {
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::DataAbortLowerEL):
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::DataAbortSameEL): {
                    auto iss = DataAbortIss::decode(exit.exception.syndrome);
                    exit.exception.is_write = iss.wnr;
                    exit.exception.access_size = iss.isv ? iss.access_size_bytes() : 4;
                    break;
                }
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::WfxTrap): {
                    // WFI/WFE trap — treat like Hlt
                    exit.exception.is_write = false;
                    exit.exception.access_size = 0;
                    break;
                }
                default:
                    exit.exception.is_write = false;
                    exit.exception.access_size = 0;
                    break;
            }
            break;
        }

        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            exit.reason = Arm64VcpuExit::Reason::VtimerActivated;
            break;

        case HV_EXIT_REASON_CANCELED:
            exit.reason = Arm64VcpuExit::Reason::Canceled;
            break;

        default:
            exit.reason = Arm64VcpuExit::Reason::Unknown;
            break;
    }

    return exit;
}

// -- General-purpose registers ------------------------------------------------

HalResult<Arm64Regs> HvfArm64Vcpu::get_regs() const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);

    Arm64Regs regs{};

    // Read X0-X30
    for (int i = 0; i <= 30; ++i) {
        hv_return_t ret = hv_vcpu_get_reg(
            vcpu_handle_,
            static_cast<hv_reg_t>(HV_REG_X0 + i),
            &regs.x[i]
        );
        if (ret != HV_SUCCESS) {
            return std::unexpected(HalError::InternalError);
        }
    }

    // PC
    if (hv_vcpu_get_reg(vcpu_handle_, HV_REG_PC, &regs.pc) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    // CPSR (PSTATE)
    if (hv_vcpu_get_reg(vcpu_handle_, HV_REG_CPSR, &regs.cpsr) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    // SP_EL1 is a system register on ARM64
    if (hv_vcpu_get_sys_reg(vcpu_handle_, HV_SYS_REG_SP_EL1, &regs.sp) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    return regs;
}

HalResult<void> HvfArm64Vcpu::set_regs(const Arm64Regs& regs) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);

    // Write X0-X30
    for (int i = 0; i <= 30; ++i) {
        hv_return_t ret = hv_vcpu_set_reg(
            vcpu_handle_,
            static_cast<hv_reg_t>(HV_REG_X0 + i),
            regs.x[i]
        );
        if (ret != HV_SUCCESS) {
            return std::unexpected(HalError::InternalError);
        }
    }

    // PC
    if (hv_vcpu_set_reg(vcpu_handle_, HV_REG_PC, regs.pc) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    // CPSR
    if (hv_vcpu_set_reg(vcpu_handle_, HV_REG_CPSR, regs.cpsr) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    // SP_EL1
    if (hv_vcpu_set_sys_reg(vcpu_handle_, HV_SYS_REG_SP_EL1, regs.sp) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    return {};
}

// -- System registers ---------------------------------------------------------

HalResult<Arm64SysRegs> HvfArm64Vcpu::get_sys_regs() const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);

    Arm64SysRegs sregs{};

    auto read = [this](hv_sys_reg_t reg, uint64_t& out) -> bool {
        return hv_vcpu_get_sys_reg(vcpu_handle_, reg, &out) == HV_SUCCESS;
    };

    if (!read(HV_SYS_REG_SCTLR_EL1,     sregs.sctlr_el1)     ||
        !read(HV_SYS_REG_TCR_EL1,        sregs.tcr_el1)        ||
        !read(HV_SYS_REG_TTBR0_EL1,      sregs.ttbr0_el1)      ||
        !read(HV_SYS_REG_TTBR1_EL1,      sregs.ttbr1_el1)      ||
        !read(HV_SYS_REG_MAIR_EL1,       sregs.mair_el1)       ||
        !read(HV_SYS_REG_VBAR_EL1,       sregs.vbar_el1)       ||
        !read(HV_SYS_REG_ESR_EL1,        sregs.esr_el1)        ||
        !read(HV_SYS_REG_FAR_EL1,        sregs.far_el1)        ||
        !read(HV_SYS_REG_ELR_EL1,        sregs.elr_el1)        ||
        !read(HV_SYS_REG_SPSR_EL1,       sregs.spsr_el1)       ||
        !read(HV_SYS_REG_SP_EL0,         sregs.sp_el0)         ||
        !read(HV_SYS_REG_TPIDR_EL0,      sregs.tpidr_el0)      ||
        !read(HV_SYS_REG_TPIDR_EL1,      sregs.tpidr_el1)      ||
        !read(HV_SYS_REG_TPIDRRO_EL0,    sregs.tpidrro_el0)    ||
        !read(HV_SYS_REG_MIDR_EL1,       sregs.midr_el1)       ||
        !read(HV_SYS_REG_MPIDR_EL1,      sregs.mpidr_el1)      ||
        !read(HV_SYS_REG_PAR_EL1,        sregs.par_el1)        ||
        !read(HV_SYS_REG_CNTV_CTL_EL0,   sregs.cntv_ctl_el0)   ||
        !read(HV_SYS_REG_CNTV_CVAL_EL0,  sregs.cntv_cval_el0)) {
        return std::unexpected(HalError::InternalError);
    }

    return sregs;
}

HalResult<void> HvfArm64Vcpu::set_sys_regs(const Arm64SysRegs& sregs) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);

    auto write = [this](hv_sys_reg_t reg, uint64_t val) -> bool {
        return hv_vcpu_set_sys_reg(vcpu_handle_, reg, val) == HV_SUCCESS;
    };

    if (!write(HV_SYS_REG_SCTLR_EL1,     sregs.sctlr_el1)     ||
        !write(HV_SYS_REG_TCR_EL1,        sregs.tcr_el1)        ||
        !write(HV_SYS_REG_TTBR0_EL1,      sregs.ttbr0_el1)      ||
        !write(HV_SYS_REG_TTBR1_EL1,      sregs.ttbr1_el1)      ||
        !write(HV_SYS_REG_MAIR_EL1,       sregs.mair_el1)       ||
        !write(HV_SYS_REG_VBAR_EL1,       sregs.vbar_el1)       ||
        !write(HV_SYS_REG_ESR_EL1,        sregs.esr_el1)        ||
        !write(HV_SYS_REG_FAR_EL1,        sregs.far_el1)        ||
        !write(HV_SYS_REG_ELR_EL1,        sregs.elr_el1)        ||
        !write(HV_SYS_REG_SPSR_EL1,       sregs.spsr_el1)       ||
        !write(HV_SYS_REG_SP_EL0,         sregs.sp_el0)         ||
        !write(HV_SYS_REG_TPIDR_EL0,      sregs.tpidr_el0)      ||
        !write(HV_SYS_REG_TPIDR_EL1,      sregs.tpidr_el1)      ||
        !write(HV_SYS_REG_TPIDRRO_EL0,    sregs.tpidrro_el0)    ||
        !write(HV_SYS_REG_PAR_EL1,        sregs.par_el1)        ||
        !write(HV_SYS_REG_CNTV_CTL_EL0,   sregs.cntv_ctl_el0)   ||
        !write(HV_SYS_REG_CNTV_CVAL_EL0,  sregs.cntv_cval_el0)) {
        return std::unexpected(HalError::InternalError);
    }

    // MIDR_EL1 and MPIDR_EL1 are typically read-only; skip writing them

    return {};
}

// -- SIMD/FP registers --------------------------------------------------------

HalResult<Arm64FpRegs> HvfArm64Vcpu::get_fp_regs() const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);

    Arm64FpRegs regs{};

    for (int i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t val;
        hv_return_t ret = hv_vcpu_get_simd_fp_reg(
            vcpu_handle_,
            static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
            &val
        );
        if (ret != HV_SUCCESS) {
            return std::unexpected(HalError::InternalError);
        }
        std::memcpy(&regs.v[i], &val, sizeof(val));
    }

    // FPCR and FPSR are system registers
    uint64_t fpcr = 0, fpsr = 0;
    if (hv_vcpu_get_sys_reg(vcpu_handle_, HV_SYS_REG_FPCR, &fpcr) != HV_SUCCESS ||
        hv_vcpu_get_sys_reg(vcpu_handle_, HV_SYS_REG_FPSR, &fpsr) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    regs.fpcr = static_cast<uint32_t>(fpcr);
    regs.fpsr = static_cast<uint32_t>(fpsr);

    return regs;
}

HalResult<void> HvfArm64Vcpu::set_fp_regs(const Arm64FpRegs& regs) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);

    for (int i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t val;
        std::memcpy(&val, &regs.v[i], sizeof(val));
        hv_return_t ret = hv_vcpu_set_simd_fp_reg(
            vcpu_handle_,
            static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
            val
        );
        if (ret != HV_SUCCESS) {
            return std::unexpected(HalError::InternalError);
        }
    }

    if (hv_vcpu_set_sys_reg(vcpu_handle_, HV_SYS_REG_FPCR, regs.fpcr) != HV_SUCCESS ||
        hv_vcpu_set_sys_reg(vcpu_handle_, HV_SYS_REG_FPSR, regs.fpsr) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    return {};
}

// -- Individual register access -----------------------------------------------

HalResult<uint64_t> HvfArm64Vcpu::get_reg(hv_reg_t reg) const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    uint64_t value = 0;
    if (hv_vcpu_get_reg(vcpu_handle_, reg, &value) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    return value;
}

HalResult<void> HvfArm64Vcpu::set_reg(hv_reg_t reg, uint64_t value) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    if (hv_vcpu_set_reg(vcpu_handle_, reg, value) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<uint64_t> HvfArm64Vcpu::get_sys_reg(hv_sys_reg_t reg) const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    uint64_t value = 0;
    if (hv_vcpu_get_sys_reg(vcpu_handle_, reg, &value) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    return value;
}

HalResult<void> HvfArm64Vcpu::set_sys_reg(hv_sys_reg_t reg, uint64_t value) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    if (hv_vcpu_set_sys_reg(vcpu_handle_, reg, value) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

// -- Interrupt injection ------------------------------------------------------

HalResult<void> HvfArm64Vcpu::set_pending_interrupt(hv_interrupt_type_t type, bool pending) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    if (hv_vcpu_set_pending_interrupt(vcpu_handle_, type, pending) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

// -- Virtual timer ------------------------------------------------------------

HalResult<void> HvfArm64Vcpu::set_vtimer_mask(bool masked) {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    if (hv_vcpu_set_vtimer_mask(vcpu_handle_, masked) != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<hv_vcpu_exit_t*> HvfArm64Vcpu::get_exit_info() const {
    if (!initialized_) return std::unexpected(HalError::NotInitialized);
    return exit_info_;
}

// ============================================================================
// HvfArm64MemoryManager — same memory mapping APIs work for both archs
// ============================================================================

class HvfArm64MemoryManager : public IMemoryManager {
public:
    HalResult<void> map_region(const MemoryRegion& region) override;
    HalResult<void> unmap_region(uint32_t slot) override;
    HalResult<HVA> gpa_to_hva(GPA gpa) const override;
    std::vector<MemoryRegion> get_regions() const override;

private:
    mutable std::mutex mutex_;
    std::vector<MemoryRegion> regions_;
};

HalResult<void> HvfArm64MemoryManager::map_region(const MemoryRegion& region) {
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

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : regions_) {
        if (r.slot == region.slot) {
            r = region;
            return {};
        }
    }
    regions_.push_back(region);
    return {};
}

HalResult<void> HvfArm64MemoryManager::unmap_region(uint32_t slot) {
    std::lock_guard<std::mutex> lock(mutex_);
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

HalResult<HVA> HvfArm64MemoryManager::gpa_to_hva(GPA gpa) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : regions_) {
        if (gpa >= r.guest_phys_addr && gpa < r.guest_phys_addr + r.size) {
            return r.userspace_addr + (gpa - r.guest_phys_addr);
        }
    }
    return std::unexpected(HalError::InvalidParameter);
}

std::vector<MemoryRegion> HvfArm64MemoryManager::get_regions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return regions_;
}

// ============================================================================
// HvfArm64Hypervisor — ARM64 hypervisor instance
// ============================================================================

class HvfArm64Hypervisor {
public:
    HvfArm64Hypervisor();
    ~HvfArm64Hypervisor();

    // Non-copyable
    HvfArm64Hypervisor(const HvfArm64Hypervisor&) = delete;
    HvfArm64Hypervisor& operator=(const HvfArm64Hypervisor&) = delete;

    std::string name() const { return "HVF-ARM64"; }
    bool is_available() const;

    HalResult<void> initialize();
    HalResult<void> create_vm(const VmConfig& config);
    HalResult<std::unique_ptr<HvfArm64Vcpu>> create_vcpu(VcpuId id);
    IMemoryManager& memory_manager();

    /// Create the in-kernel GIC (Generic Interrupt Controller)
    /// Available on macOS 13.0+ with Apple Silicon
    HalResult<void> create_gic();

    /// Set an IRQ line on the GIC
    HalResult<void> set_irq_line(uint32_t irq, bool level);

    int api_version() const { return 1; }

private:
    std::unique_ptr<HvfArm64MemoryManager> mem_mgr_;
    VmConfig config_;
    bool vm_created_ = false;
    bool gic_created_ = false;
};

// ============================================================================
// HvfArm64Hypervisor implementation
// ============================================================================

HvfArm64Hypervisor::HvfArm64Hypervisor() = default;

HvfArm64Hypervisor::~HvfArm64Hypervisor() {
    if (vm_created_) {
        hv_vm_destroy();
    }
}

bool HvfArm64Hypervisor::is_available() const {
    hv_return_t ret = hv_vm_create(HV_VM_DEFAULT);
    if (ret == HV_SUCCESS) {
        hv_vm_destroy();
        return true;
    }
    return false;
}

HalResult<void> HvfArm64Hypervisor::initialize() {
    return {};
}

HalResult<void> HvfArm64Hypervisor::create_vm(const VmConfig& config) {
    if (vm_created_) {
        return std::unexpected(HalError::AlreadyExists);
    }

    config_ = config;

    hv_return_t ret = hv_vm_create(HV_VM_DEFAULT);
    if (ret != HV_SUCCESS) {
        return std::unexpected(HalError::InternalError);
    }

    vm_created_ = true;
    mem_mgr_ = std::make_unique<HvfArm64MemoryManager>();
    return {};
}

HalResult<std::unique_ptr<HvfArm64Vcpu>> HvfArm64Hypervisor::create_vcpu(VcpuId id) {
    if (!vm_created_) {
        return std::unexpected(HalError::NotInitialized);
    }

    auto vcpu = std::make_unique<HvfArm64Vcpu>(id);
    if (!vcpu->is_valid()) {
        return std::unexpected(HalError::InternalError);
    }

    return vcpu;
}

IMemoryManager& HvfArm64Hypervisor::memory_manager() {
    return *mem_mgr_;
}

HalResult<void> HvfArm64Hypervisor::create_gic() {
    if (!vm_created_) {
        return std::unexpected(HalError::NotInitialized);
    }

    if (gic_created_) {
        return std::unexpected(HalError::AlreadyExists);
    }

    // macOS 14.0+ provides hv_gic_create() for in-kernel GICv3 emulation.
    // The GIC distributor and redistributor are automatically mapped.
    //
    // On older macOS versions, GIC must be emulated in userspace.
    // We attempt the kernel GIC first and fall back gracefully.
    if (__builtin_available(macOS 14.0, *)) {
        // hv_gic_config_t allows setting distributor/redistributor base addrs
        hv_gic_config_t gic_config = hv_gic_config_create();

        // Standard GICv3 memory map for AOSP:
        //   GICD base: 0x08000000 (distributor)
        //   GICR base: 0x080A0000 (redistributor, per-CPU)
        hv_gic_config_set_distributor_base(gic_config, 0x08000000);
        hv_gic_config_set_redistributor_base(gic_config, 0x080A0000);

        // MSI support: set ITS base if needed for PCI passthrough
        // hv_gic_config_set_msi_region_base(gic_config, 0x08020000);
        // hv_gic_config_set_msi_region_size(gic_config, 0x100000);

        hv_return_t ret = hv_gic_create(gic_config);
        os_release(gic_config);

        if (ret != HV_SUCCESS) {
            return std::unexpected(HalError::InternalError);
        }

        gic_created_ = true;
        return {};
    }

    // Fallback: no kernel GIC available; VMM must emulate
    return std::unexpected(HalError::NotSupported);
}

HalResult<void> HvfArm64Hypervisor::set_irq_line(uint32_t irq, bool level) {
    if (!vm_created_) {
        return std::unexpected(HalError::NotInitialized);
    }

    if (!gic_created_) {
        // Without kernel GIC, interrupt routing must be handled by VMM layer
        return std::unexpected(HalError::NotSupported);
    }

    if (__builtin_available(macOS 14.0, *)) {
        // Use hv_gic_set_spi to signal a Shared Peripheral Interrupt
        hv_return_t ret = hv_gic_set_spi(irq, level);
        if (ret != HV_SUCCESS) {
            return std::unexpected(HalError::InternalError);
        }
        return {};
    }

    return std::unexpected(HalError::NotSupported);
}

// ============================================================================
// Utility: convert Arm64VcpuExit to generic VcpuExit for cross-arch VMM use
// ============================================================================

/// Convert an ARM64-specific exit into the generic VcpuExit representation.
/// This allows upper VMM layers to handle exits uniformly across architectures.
VcpuExit arm64_exit_to_generic(const Arm64VcpuExit& arm_exit) {
    VcpuExit exit{};

    switch (arm_exit.reason) {
        case Arm64VcpuExit::Reason::Exception: {
            auto ec = arm_exit.exception_class();
            switch (static_cast<uint8_t>(ec)) {
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::DataAbortLowerEL):
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::DataAbortSameEL): {
                    if (arm_exit.is_translation_fault()) {
                        // Translation fault on unmapped IPA → MMIO access
                        exit.reason = VcpuExit::Reason::MmioAccess;
                        exit.mmio.address = arm_exit.exception.pa;
                        exit.mmio.is_write = arm_exit.exception.is_write;
                        exit.mmio.size = arm_exit.exception.access_size;
                        exit.mmio.data = 0;
                    } else {
                        exit.reason = VcpuExit::Reason::InternalError;
                    }
                    break;
                }
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::WfxTrap):
                    exit.reason = VcpuExit::Reason::Hlt;
                    break;
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::HvcAarch64):
                case static_cast<uint8_t>(Arm64VcpuExit::ExceptionClass::SmcAarch64):
                    // HVC/SMC calls — could be PSCI or other firmware calls
                    exit.reason = VcpuExit::Reason::Unknown;
                    break;
                default:
                    exit.reason = VcpuExit::Reason::Unknown;
                    break;
            }
            break;
        }

        case Arm64VcpuExit::Reason::VtimerActivated:
            exit.reason = VcpuExit::Reason::IrqWindowOpen;
            break;

        case Arm64VcpuExit::Reason::Canceled:
            exit.reason = VcpuExit::Reason::Hlt;
            break;

        case Arm64VcpuExit::Reason::IrqPending:
            exit.reason = VcpuExit::Reason::IrqWindowOpen;
            break;

        default:
            exit.reason = VcpuExit::Reason::Unknown;
            break;
    }

    return exit;
}

} // namespace rex::hal

#endif // defined(__APPLE__) && defined(__aarch64__)
