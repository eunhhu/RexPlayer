#if defined(__APPLE__) && defined(__aarch64__)

#include "hvf_arm64_adapter.h"

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vcpu.h>
#include <Hypervisor/hv_vcpu_config.h>
#include <Hypervisor/hv_vcpu_types.h>
#include <Hypervisor/hv_vm.h>
#include <Hypervisor/hv_vm_types.h>
#include <algorithm>
#include <atomic>
#include <cstring>

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

        // Set HCR_EL2 for proper EL1 guest trapping:
        //   VM(0)  — stage-2 translation (HVF manages this, but MMIO needs it)
        //   SWIO(1) — set/way invalidation override
        //   FMO(3) — FIQ to EL2
        //   IMO(4) — IRQ to EL2
        //   AMO(5) — SError to EL2
        //   TWI(27) — trap WFI
        //   TWE(28) — trap WFE
        //   RW(31) — EL1 is AArch64
        //   TSC(19) — trap SMC
        uint64_t hcr = (1ULL << 0)   // VM
                     | (1ULL << 1)   // SWIO
                     | (1ULL << 3)   // FMO
                     | (1ULL << 4)   // IMO
                     | (1ULL << 5)   // AMO
                     | (1ULL << 19)  // TSC (trap SMC)
                     | (1ULL << 27)  // TWI
                     | (1ULL << 28)  // TWE
                     | (1ULL << 31); // RW
        hv_vcpu_set_sys_reg(handle, HV_SYS_REG_HCR_EL2, hcr);

        // Enable vtimer delivery (unmask)
        hv_return_t vtimer_ret = hv_vcpu_set_vtimer_mask(handle, false);
        fprintf(stderr, "vcpu %u: vtimer unmask result=%d\n", id, vtimer_ret);

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

// PSCI function IDs (SMCCC convention)
constexpr uint32_t PSCI_VERSION       = 0x84000000;
constexpr uint32_t PSCI_CPU_SUSPEND   = 0xC4000001;
constexpr uint32_t PSCI_CPU_OFF       = 0x84000002;
constexpr uint32_t PSCI_CPU_ON        = 0xC4000003;
constexpr uint32_t PSCI_SYSTEM_OFF    = 0x84000008;
constexpr uint32_t PSCI_SYSTEM_RESET  = 0x84000009;
constexpr uint32_t PSCI_FEATURES      = 0x8400000A;
constexpr uint32_t SMCCC_VERSION      = 0x80000000;
constexpr uint32_t SMCCC_ARCH_FEATURES = 0x80000001;

constexpr int32_t PSCI_SUCCESS        = 0;
constexpr int32_t PSCI_NOT_SUPPORTED  = -1;
constexpr int32_t PSCI_ALREADY_ON     = -6;

bool handle_psci_hvc(HvfArm64VcpuImpl& impl) {
    uint64_t pc = 0, x0 = 0;
    hv_vcpu_get_reg(impl.handle, HV_REG_PC, &pc);
    hv_vcpu_get_reg(impl.handle, HV_REG_X0, &x0);

    uint32_t func_id = static_cast<uint32_t>(x0);
    int64_t ret_val = PSCI_NOT_SUPPORTED;

    switch (func_id) {
        case PSCI_VERSION:
            // PSCI 1.0
            ret_val = (1 << 16) | 0; // major=1, minor=0
            break;
        case PSCI_FEATURES: {
            // X1 contains the function ID to query
            uint64_t x1 = 0;
            hv_vcpu_get_reg(impl.handle, HV_REG_X1, &x1);
            uint32_t query_id = static_cast<uint32_t>(x1);
            switch (query_id) {
                case PSCI_VERSION:
                case PSCI_CPU_OFF:
                case PSCI_CPU_ON:
                case PSCI_SYSTEM_OFF:
                case PSCI_SYSTEM_RESET:
                case PSCI_CPU_SUSPEND:
                    ret_val = PSCI_SUCCESS;
                    break;
                default:
                    ret_val = PSCI_NOT_SUPPORTED;
                    break;
            }
            break;
        }
        case PSCI_CPU_ON:
            // Single vCPU — report already on
            ret_val = PSCI_ALREADY_ON;
            break;
        case PSCI_CPU_OFF:
            ret_val = PSCI_SUCCESS;
            break;
        case PSCI_SYSTEM_OFF:
        case PSCI_SYSTEM_RESET:
            ret_val = PSCI_SUCCESS;
            break;
        case PSCI_CPU_SUSPEND:
            ret_val = PSCI_SUCCESS;
            break;
        case SMCCC_VERSION:
            // SMCCC 1.1
            ret_val = (1 << 16) | 1;
            break;
        case SMCCC_ARCH_FEATURES:
            ret_val = PSCI_NOT_SUPPORTED;
            break;
        default:
            ret_val = PSCI_NOT_SUPPORTED;
            break;
    }

    static std::atomic<int> hvc_log_count{0};
    const auto hvc_log_index = hvc_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hvc_log_index <= 20) {
        uint64_t cur_hcr = 0;
        hv_vcpu_get_sys_reg(impl.handle, HV_SYS_REG_HCR_EL2, &cur_hcr);
        fprintf(stderr, "HVC: func=0x%08x → ret=0x%llx pc=0x%llx HCR=0x%llx\n",
                func_id, static_cast<uint64_t>(ret_val), pc, cur_hcr);
    }

    // SMCCC: return values in X0-X3
    // NOTE: HVF already advances PC past the HVC instruction
    hv_vcpu_set_reg(impl.handle, HV_REG_X0, static_cast<uint64_t>(ret_val));
    hv_vcpu_set_reg(impl.handle, HV_REG_X1, 0);
    hv_vcpu_set_reg(impl.handle, HV_REG_X2, 0);
    hv_vcpu_set_reg(impl.handle, HV_REG_X3, 0);
    // Do NOT advance PC — HVF does it automatically for HVC/SMC traps
    return true;
}

/// Minimal GICv3 CPU-interface (ICC) system register emulation.
/// The kernel accesses these via MSR/MRS (ec=0x18) instead of MMIO.
/// We emulate just enough so the GIC driver initialises and the
/// timer/IRQ path works.
struct IccState {
    uint64_t pmr      = 0;          // ICC_PMR_EL1: priority mask
    uint64_t ctlr     = 0;          // ICC_CTLR_EL1
    uint64_t igrpen1  = 0;          // ICC_IGRPEN1_EL1
    uint64_t bpr1     = 0;          // ICC_BPR1_EL1
    uint64_t sre      = 0x7;        // ICC_SRE_EL1: SRE|DFB|DIB (system regs enabled)
    uint64_t ap0r[4]  = {};         // ICC_AP0R<n>_EL1
    uint64_t ap1r[4]  = {};         // ICC_AP1R<n>_EL1
};

/// Decode ISS for EC=0x18 and handle ICC_* system register accesses.
/// Returns true if the access was handled (PC advanced), false otherwise.
bool handle_icc_sysreg(HvfArm64VcpuImpl& impl, uint32_t syndrome, IccState& icc) {
    uint32_t iss = syndrome & 0x1FFFFFF;
    // QEMU encoding: op0[21:20] op2[19:17] op1[16:14] crn[13:10] rt[9:5] crm[4:1] dir[0]
    uint32_t op0 = (iss >> 20) & 0x3;
    uint32_t op2 = (iss >> 17) & 0x7;
    uint32_t op1 = (iss >> 14) & 0x7;
    uint32_t crn = (iss >> 10) & 0xF;
    uint32_t rt  = (iss >> 5)  & 0x1F;
    uint32_t crm = (iss >> 1)  & 0xF;
    bool is_read = iss & 1;

    // Only handle S3_0_C*_C*_* (Op0=3, Op1=0 — EL1 ICC regs)
    if (op0 != 3) return false;

    // Encode a compact key: (op1 << 12) | (crn << 8) | (crm << 4) | op2
    uint32_t key = (op1 << 12) | (crn << 8) | (crm << 4) | op2;

    // Key values for known ICC registers (Op0=3 implied):
    // ICC_PMR_EL1       = S3_0_C4_C6_0   → key = 0x0460
    // ICC_IAR0_EL1      = S3_0_C12_C8_0  → key = 0x0C80
    // ICC_IAR1_EL1      = S3_0_C12_C12_0 → key = 0x0CC0
    // ICC_EOIR0_EL1     = S3_0_C12_C8_1  → key = 0x0C81
    // ICC_EOIR1_EL1     = S3_0_C12_C12_1 → key = 0x0CC1
    // ICC_HPPIR1_EL1    = S3_0_C12_C12_2 → key = 0x0CC2
    // ICC_BPR0_EL1      = S3_0_C12_C8_3  → key = 0x0C83
    // ICC_BPR1_EL1      = S3_0_C12_C12_3 → key = 0x0CC3
    // ICC_CTLR_EL1      = S3_0_C12_C12_4 → key = 0x0CC4
    // ICC_SRE_EL1       = S3_0_C12_C12_5 → key = 0x0CC5
    // ICC_IGRPEN0_EL1   = S3_0_C12_C12_6 → key = 0x0CC6
    // ICC_IGRPEN1_EL1   = S3_0_C12_C12_7 → key = 0x0CC7
    // ICC_SGI1R_EL1     = S3_0_C12_C11_5 → key = 0x0CB5
    // ICC_DIR_EL1       = S3_0_C12_C11_1 → key = 0x0CB1
    // ICC_RPR_EL1       = S3_0_C12_C11_3 → key = 0x0CB3
    // ICC_AP0R<n>_EL1   = S3_0_C12_C8_4+n → key = 0x0C84..0x0C87
    // ICC_AP1R<n>_EL1   = S3_0_C12_C9_0+n → key = 0x0C90..0x0C93

    uint64_t val = 0;
    bool handled = true;

    if (is_read) {
        switch (key) {
            case 0x0460: val = icc.pmr; break;              // ICC_PMR_EL1
            case 0x0CC0: val = 0x3FF; break;                // ICC_IAR1_EL1: spurious (1023)
            case 0x0C80: val = 0x3FF; break;                // ICC_IAR0_EL1: spurious
            case 0x0CC2: val = 0x3FF; break;                // ICC_HPPIR1_EL1: no pending
            case 0x0CC3: val = icc.bpr1; break;             // ICC_BPR1_EL1
            case 0x0C83: val = 0; break;                    // ICC_BPR0_EL1
            case 0x0CC4: val = icc.ctlr; break;             // ICC_CTLR_EL1
            case 0x0CC5: val = icc.sre; break;              // ICC_SRE_EL1
            case 0x0CC6: val = 0; break;                    // ICC_IGRPEN0_EL1
            case 0x0CC7: val = icc.igrpen1; break;          // ICC_IGRPEN1_EL1
            case 0x0CB3: val = 0xFF; break;                 // ICC_RPR_EL1: idle priority
            // ICC_AP0R<n>_EL1 (n=0..3)
            case 0x0C84: case 0x0C85: case 0x0C86: case 0x0C87:
                val = icc.ap0r[key - 0x0C84]; break;
            // ICC_AP1R<n>_EL1 (n=0..3)
            case 0x0C90: case 0x0C91: case 0x0C92: case 0x0C93:
                val = icc.ap1r[key - 0x0C90]; break;
            default:
                handled = false;
                break;
        }
        if (handled && rt < 31) {
            hv_vcpu_set_reg(impl.handle,
                static_cast<hv_reg_t>(HV_REG_X0 + rt), val);
        }
    } else {
        // Write: read value from Rt
        if (rt < 31)
            hv_vcpu_get_reg(impl.handle,
                static_cast<hv_reg_t>(HV_REG_X0 + rt), &val);
        else
            val = 0; // XZR

        switch (key) {
            case 0x0460: icc.pmr = val; break;              // ICC_PMR_EL1
            case 0x0CC1: break;                              // ICC_EOIR1_EL1: EOI (ignore)
            case 0x0C81: break;                              // ICC_EOIR0_EL1: EOI (ignore)
            case 0x0CC3: icc.bpr1 = val; break;             // ICC_BPR1_EL1
            case 0x0C83: break;                              // ICC_BPR0_EL1 (ignore)
            case 0x0CC4: icc.ctlr = val; break;             // ICC_CTLR_EL1
            case 0x0CC5: break;                              // ICC_SRE_EL1: read-only (ignore write)
            case 0x0CC6: break;                              // ICC_IGRPEN0_EL1 (ignore)
            case 0x0CC7: icc.igrpen1 = val; break;          // ICC_IGRPEN1_EL1
            case 0x0CB1: break;                              // ICC_DIR_EL1: deactivate (ignore)
            case 0x0CB5: break;                              // ICC_SGI1R_EL1: SGI (ignore for now)
            // ICC_AP0R<n>_EL1
            case 0x0C84: case 0x0C85: case 0x0C86: case 0x0C87:
                icc.ap0r[key - 0x0C84] = val; break;
            // ICC_AP1R<n>_EL1
            case 0x0C90: case 0x0C91: case 0x0C92: case 0x0C93:
                icc.ap1r[key - 0x0C90] = val; break;
            default:
                handled = false;
                break;
        }
    }

    if (handled) {
        // Advance PC past the MSR/MRS instruction (always 4 bytes in AArch64)
        uint64_t pc = 0;
        hv_vcpu_get_reg(impl.handle, HV_REG_PC, &pc);
        hv_vcpu_set_reg(impl.handle, HV_REG_PC, pc + 4);

        static std::atomic<uint64_t> icc_log_count{0};
        auto idx = icc_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (idx <= 30 || idx % 10000 == 0) {
            fprintf(stderr, "ICC #%llu: %s S3_%u_C%u_C%u_%u (key=0x%04x) x%u=0x%llx pc=0x%llx\n",
                    static_cast<unsigned long long>(idx),
                    is_read ? "MRS" : "MSR",
                    op1, crn, crm, op2, key, rt,
                    static_cast<unsigned long long>(val), pc);
        }
    }

    return handled;
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

    static IccState icc_state;  // Per-vCPU ICC register state

    for (;;) {
        hv_return_t ret = hv_vcpu_run(impl_->handle);
        if (ret != HV_SUCCESS) {
            fprintf(stderr, "hv_vcpu_run failed: %d\n", ret);
            return std::unexpected(HalError::VcpuRunFailed);
        }

        VcpuExit exit{};
        auto* ei = impl_->exit_info;
        const bool is_exception = ei->reason == HV_EXIT_REASON_EXCEPTION;
        const auto exception_class = is_exception
            ? static_cast<unsigned long long>((ei->exception.syndrome >> 26) & 0x3F)
            : 0ULL;
        const bool is_idle_exit = is_exception && exception_class == 0x01;

        static std::atomic<uint64_t> exit_count{0};
        const auto exit_index = exit_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!is_idle_exit && (exit_index <= 100 || exit_index % 10000 == 0)) {
            uint64_t pc = 0;
            hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
            fprintf(stderr, "vcpu exit #%llu: reason=%d pc=0x%llx",
                    static_cast<unsigned long long>(exit_index), ei->reason, pc);
            if (is_exception) {
                const auto syndrome =
                    static_cast<unsigned long long>(ei->exception.syndrome);
                fprintf(stderr, " syndrome=0x%llx ec=0x%llx", syndrome, exception_class);
            }
            fprintf(stderr, "\n");
        }

        switch (ei->reason) {
            case HV_EXIT_REASON_EXCEPTION: {
                uint32_t syndrome = ei->exception.syndrome;
                uint8_t ec = (syndrome >> 26) & 0x3F;

                if (ec == 0x24 || ec == 0x25) { // Data abort → MMIO
                    uint64_t pa = ei->exception.physical_address;
                    bool isv = (syndrome >> 24) & 1;
                    bool wnr = (syndrome >> 6) & 1;
                    uint8_t sas = (syndrome >> 22) & 3;
                    uint32_t srt = isv ? ((syndrome >> 16) & 0x1F) : 0;
                    uint8_t access_size = isv ? static_cast<uint8_t>(1 << sas) : 4;

                    // Handle MMIO internally for known devices
                    bool handled = false;

                    // --- PL011 UART (0x09000000) ---
                    if ((pa & ~0xFFFULL) == 0x09000000) {
                        uint32_t off = static_cast<uint32_t>(pa & 0xFFF);
                        if (wnr) {
                            if (off == 0x00) { // UARTDR — write char
                                uint64_t val = 0;
                                if (isv) hv_vcpu_get_reg(impl_->handle,
                                    static_cast<hv_reg_t>(HV_REG_X0 + srt), &val);
                                fputc(static_cast<char>(val & 0xFF), stderr);
                            }
                        } else {
                            uint64_t resp = 0;
                            switch (off) {
                                case 0x18: resp = 0x10; break; // UARTFR: TXFE (bit4)
                                case 0x00: resp = 0; break;    // UARTDR: no data
                                default:   resp = 0; break;
                            }
                            if (isv) hv_vcpu_set_reg(impl_->handle,
                                static_cast<hv_reg_t>(HV_REG_X0 + srt), resp);
                        }
                        handled = true;
                    }
                    // --- GIC distributor (0x08000000) + redistributor (0x080A0000) ---
                    else if (pa >= 0x08000000 && pa < 0x081A0000) {
                        uint64_t resp = 0;
                        uint32_t off = static_cast<uint32_t>(pa - 0x08000000);

                        if (!wnr) {
                            // --- GICD (distributor) registers: offset 0x0000-0xFFFF ---
                            if (off < 0xA0000) {
                                switch (off) {
                                    case 0x0000: resp = (1 << 6) | (1 << 4) | (1 << 1) | 1; break; // GICD_CTLR: DS | ARE_NS | EnableGrp1NS | EnableGrp0
                                    case 0x0004: // GICD_TYPER
                                        resp = (3ULL << 19)  // IDbits=15 (16-bit INTID)
                                             | (0ULL << 11)  // SecurityExtn=0
                                             | (0ULL << 5)   // CPUNumber=0
                                             | 4;            // ITLinesNumber=4 (160 IRQs)
                                        break;
                                    case 0x0008: resp = (0x43B << 0); break; // GICD_IIDR: ARM
                                    case 0xFFE8: resp = 0x3B; break; // GICD_PIDR2: ArchRev=3 (GICv3)
                                    default:
                                        // GICD_IGROUPR, ISENABLER, etc — return 0
                                        resp = 0;
                                        break;
                                }
                            }
                            // --- GICR (redistributor) registers: offset 0xA0000+ ---
                            else {
                                uint32_t rd_off = off - 0xA0000;
                                // Each redistributor frame is 128KB (0x20000):
                                //   0x00000-0x0FFFF: RD_base
                                //   0x10000-0x1FFFF: SGI_base
                                uint32_t frame_off = rd_off % 0x20000;
                                switch (frame_off) {
                                    case 0x0000: // GICR_CTLR
                                        resp = 0;
                                        break;
                                    case 0x0004: // GICR_IIDR
                                        resp = 0x43B;
                                        break;
                                    case 0x0008: // GICR_TYPER (low 32 bits)
                                        resp = (1 << 4); // Last=1 (single CPU)
                                        break;
                                    case 0x000C: // GICR_TYPER (high 32 bits)
                                        resp = 0;
                                        break;
                                    case 0x0014: // GICR_WAKER
                                        resp = 0; // ProcessorSleep=0, ChildrenAsleep=0
                                        break;
                                    case 0xFFE8: // GICR_PIDR2
                                        resp = 0x3B; // ArchRev=3
                                        break;
                                    // SGI_base registers (frame_off >= 0x10000)
                                    case 0x10080: // GICR_IGROUPR0
                                        resp = 0xFFFFFFFF; // All Group 1
                                        break;
                                    case 0x10100: // GICR_ISENABLER0
                                        resp = 0;
                                        break;
                                    case 0x10400: // GICR_IPRIORITYR0
                                        resp = 0;
                                        break;
                                    case 0x10C00: // GICR_ICFGR0
                                        resp = 0;
                                        break;
                                    default:
                                        resp = 0;
                                        break;
                                }
                            }
                            if (isv) hv_vcpu_set_reg(impl_->handle,
                                static_cast<hv_reg_t>(HV_REG_X0 + srt), resp);
                        }
                        // Writes are accepted and ignored (minimal stub)
                        handled = true;
                    }

                    // Advance PC
                    uint64_t pc = 0;
                    hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
                    hv_vcpu_set_reg(impl_->handle, HV_REG_PC, pc + 4);

                    if (handled) {
                        continue; // Don't return to VMM, keep running
                    }

                    // Unknown MMIO — return to VMM
                    exit.reason = VcpuExit::Reason::MmioAccess;
                    exit.mmio.address = pa;
                    exit.mmio.size = access_size;
                    exit.mmio.is_write = wnr;
                    if (wnr && isv) {
                        uint64_t val = 0;
                        hv_vcpu_get_reg(impl_->handle,
                            static_cast<hv_reg_t>(HV_REG_X0 + srt), &val);
                        exit.mmio.data = val;
                    }
                } else if (ec == 0x01) { // WFI/WFE → Hlt
                    // Unmask vtimer before returning to VMM so the next
                    // timer interrupt can fire and wake us from WFI
                    hv_vcpu_set_vtimer_mask(impl_->handle, false);
                    exit.reason = VcpuExit::Reason::Hlt;
                } else if (ec == 0x18) { // MSR/MRS trap → ICC sysreg emulation
                    if (handle_icc_sysreg(*impl_, syndrome, icc_state)) {
                        continue; // Handled, keep running
                    }
                    // Unhandled sysreg — log and skip
                    {
                        uint32_t iss = syndrome & 0x1FFFFFF;
                        uint32_t s_op0 = (iss >> 20) & 0x3;
                        uint32_t s_op2 = (iss >> 17) & 0x7;
                        uint32_t s_op1 = (iss >> 14) & 0x7;
                        uint32_t s_crn = (iss >> 10) & 0xF;
                        uint32_t s_rt  = (iss >> 5)  & 0x1F;
                        uint32_t s_crm = (iss >> 1)  & 0xF;
                        bool s_read = iss & 1;
                        static std::atomic<uint64_t> unk_sysreg_count{0};
                        auto idx = unk_sysreg_count.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (idx <= 20 || idx % 10000 == 0) {
                            uint64_t pc = 0;
                            hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
                            fprintf(stderr, "SYSREG UNKNOWN #%llu: %s S%u_%u_C%u_C%u_%u rt=x%u pc=0x%llx\n",
                                    static_cast<unsigned long long>(idx),
                                    s_read ? "MRS" : "MSR",
                                    s_op0, s_op1, s_crn, s_crm, s_op2,
                                    s_rt, static_cast<unsigned long long>(pc));
                        }
                        // Return 0 for reads, ignore writes, advance PC
                        if (s_read && s_rt < 31) {
                            hv_vcpu_set_reg(impl_->handle,
                                static_cast<hv_reg_t>(HV_REG_X0 + s_rt), 0);
                        }
                        uint64_t pc = 0;
                        hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
                        hv_vcpu_set_reg(impl_->handle, HV_REG_PC, pc + 4);
                        continue;
                    }
                } else if (ec == 0x16 || ec == 0x17) { // HVC/SMC
                    // Treat trapped firmware calls as "not supported" and
                    // advance past the instruction so guests are not killed
                    // before the VMM grows real PSCI/SMCCC handling.
                    if (!handle_psci_hvc(*impl_)) {
                        return std::unexpected(HalError::InternalError);
                    }
                    continue;
                } else {
                    exit.reason = VcpuExit::Reason::Unknown;
                }
                break;
            }

            case HV_EXIT_REASON_VTIMER_ACTIVATED: {
                // vtimer fired: inject IRQ into the guest and mask the vtimer
                // until the guest re-arms it. The guest's timer IRQ handler will
                // clear CNTV_CTL_EL0.ISTATUS by writing a new CNTV_TVAL, which
                // re-arms the timer. We unmask on WFI so the next fire is
                // delivered.
                static std::atomic<uint64_t> vtimer_count{0};
                const auto vt_idx = vtimer_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if (vt_idx <= 20 || vt_idx % 10000 == 0) {
                    uint64_t pc = 0;
                    hv_vcpu_get_reg(impl_->handle, HV_REG_PC, &pc);
                    fprintf(stderr, "VTIMER #%llu: injecting IRQ, pc=0x%llx\n",
                            static_cast<unsigned long long>(vt_idx), pc);
                }
                // 1) Mask vtimer so HVF stops re-firing until we unmask
                hv_vcpu_set_vtimer_mask(impl_->handle, true);
                // 2) Inject IRQ into the guest so it takes the timer interrupt
                hv_vcpu_set_pending_interrupt(impl_->handle, HV_INTERRUPT_TYPE_IRQ, true);
                // 3) Continue running — don't return to VMM
                continue;
            }
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
