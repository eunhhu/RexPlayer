#ifdef _WIN32

#include "whpx_hypervisor.h"

#include <windows.h>
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>
#include <cstring>
#include <algorithm>

namespace rex::hal {

// ============================================================================
// WhpxVcpu
// ============================================================================

WhpxVcpu::WhpxVcpu(WHV_PARTITION_HANDLE partition, VcpuId id)
    : id_(id), partition_(partition) {}

WhpxVcpu::~WhpxVcpu() {
    if (partition_) {
        WHvDeleteVirtualProcessor(partition_, id_);
    }
}

HalResult<VcpuExit> WhpxVcpu::run() {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    WHV_RUN_VP_EXIT_CONTEXT exit_ctx{};
    HRESULT hr = WHvRunVirtualProcessor(
        partition_, id_, &exit_ctx, sizeof(exit_ctx));
    if (FAILED(hr)) {
        return std::unexpected(HalError::VcpuRunFailed);
    }

    VcpuExit exit{};

    switch (exit_ctx.ExitReason) {
        case WHvRunVpExitReasonMemoryAccess: {
            exit.reason = VcpuExit::Reason::MmioAccess;
            const auto& mem = exit_ctx.MemoryAccess;
            exit.mmio.address = mem.Gpa;
            exit.mmio.size = static_cast<uint8_t>(mem.AccessInfo.AccessSize);
            exit.mmio.is_write =
                (mem.AccessInfo.AccessType == WHvMemoryAccessWrite);
            if (exit.mmio.is_write) {
                std::memcpy(&exit.mmio.data, mem.Data, exit.mmio.size);
            }
            break;
        }

        case WHvRunVpExitReasonX64IoPortAccess: {
            exit.reason = VcpuExit::Reason::IoAccess;
            const auto& io = exit_ctx.IoPortAccess;
            exit.io.port = io.PortNumber;
            exit.io.size = static_cast<uint8_t>(io.AccessInfo.AccessSize);
            exit.io.is_write = io.AccessInfo.IsWrite;
            if (exit.io.is_write) {
                std::memcpy(&exit.io.data, &io.Rax, exit.io.size);
            }
            break;
        }

        case WHvRunVpExitReasonX64Halt:
            exit.reason = VcpuExit::Reason::Hlt;
            break;

        case WHvRunVpExitReasonX64InterruptWindow:
            exit.reason = VcpuExit::Reason::IrqWindowOpen;
            break;

        case WHvRunVpExitReasonCanceled:
            // Canceled is used when the vCPU is paused externally
            exit.reason = VcpuExit::Reason::Unknown;
            break;

        case WHvRunVpExitReasonNone:
            exit.reason = VcpuExit::Reason::Unknown;
            break;

        case WHvRunVpExitReasonInvalidVpRegisterValue:
        case WHvRunVpExitReasonUnrecoverableException:
            exit.reason = VcpuExit::Reason::InternalError;
            break;

        default:
            exit.reason = VcpuExit::Reason::Unknown;
            break;
    }

    return exit;
}

HalResult<X86Regs> WhpxVcpu::get_regs() const {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    // 18 general-purpose registers + RIP + RFLAGS
    static constexpr WHV_REGISTER_NAME reg_names[] = {
        WHvX64RegisterRax,
        WHvX64RegisterRbx,
        WHvX64RegisterRcx,
        WHvX64RegisterRdx,
        WHvX64RegisterRsi,
        WHvX64RegisterRdi,
        WHvX64RegisterRbp,
        WHvX64RegisterRsp,
        WHvX64RegisterR8,
        WHvX64RegisterR9,
        WHvX64RegisterR10,
        WHvX64RegisterR11,
        WHvX64RegisterR12,
        WHvX64RegisterR13,
        WHvX64RegisterR14,
        WHvX64RegisterR15,
        WHvX64RegisterRip,
        WHvX64RegisterRflags,
    };
    static constexpr UINT32 reg_count =
        static_cast<UINT32>(std::size(reg_names));

    WHV_REGISTER_VALUE values[reg_count]{};
    HRESULT hr = WHvGetVirtualProcessorRegisters(
        partition_, id_, reg_names, reg_count, values);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }

    X86Regs regs{};
    regs.rax    = values[0].Reg64;
    regs.rbx    = values[1].Reg64;
    regs.rcx    = values[2].Reg64;
    regs.rdx    = values[3].Reg64;
    regs.rsi    = values[4].Reg64;
    regs.rdi    = values[5].Reg64;
    regs.rbp    = values[6].Reg64;
    regs.rsp    = values[7].Reg64;
    regs.r8     = values[8].Reg64;
    regs.r9     = values[9].Reg64;
    regs.r10    = values[10].Reg64;
    regs.r11    = values[11].Reg64;
    regs.r12    = values[12].Reg64;
    regs.r13    = values[13].Reg64;
    regs.r14    = values[14].Reg64;
    regs.r15    = values[15].Reg64;
    regs.rip    = values[16].Reg64;
    regs.rflags = values[17].Reg64;

    return regs;
}

HalResult<void> WhpxVcpu::set_regs(const X86Regs& regs) {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    static constexpr WHV_REGISTER_NAME reg_names[] = {
        WHvX64RegisterRax,
        WHvX64RegisterRbx,
        WHvX64RegisterRcx,
        WHvX64RegisterRdx,
        WHvX64RegisterRsi,
        WHvX64RegisterRdi,
        WHvX64RegisterRbp,
        WHvX64RegisterRsp,
        WHvX64RegisterR8,
        WHvX64RegisterR9,
        WHvX64RegisterR10,
        WHvX64RegisterR11,
        WHvX64RegisterR12,
        WHvX64RegisterR13,
        WHvX64RegisterR14,
        WHvX64RegisterR15,
        WHvX64RegisterRip,
        WHvX64RegisterRflags,
    };
    static constexpr UINT32 reg_count =
        static_cast<UINT32>(std::size(reg_names));

    WHV_REGISTER_VALUE values[reg_count]{};
    values[0].Reg64  = regs.rax;
    values[1].Reg64  = regs.rbx;
    values[2].Reg64  = regs.rcx;
    values[3].Reg64  = regs.rdx;
    values[4].Reg64  = regs.rsi;
    values[5].Reg64  = regs.rdi;
    values[6].Reg64  = regs.rbp;
    values[7].Reg64  = regs.rsp;
    values[8].Reg64  = regs.r8;
    values[9].Reg64  = regs.r9;
    values[10].Reg64 = regs.r10;
    values[11].Reg64 = regs.r11;
    values[12].Reg64 = regs.r12;
    values[13].Reg64 = regs.r13;
    values[14].Reg64 = regs.r14;
    values[15].Reg64 = regs.r15;
    values[16].Reg64 = regs.rip;
    values[17].Reg64 = regs.rflags;

    HRESULT hr = WHvSetVirtualProcessorRegisters(
        partition_, id_, reg_names, reg_count, values);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

// Helper: convert our X86Segment to a WHV_X64_SEGMENT_REGISTER
static WHV_X64_SEGMENT_REGISTER to_whv_segment(const X86Segment& seg) {
    WHV_X64_SEGMENT_REGISTER ws{};
    ws.Base = seg.base;
    ws.Limit = seg.limit;
    ws.Selector = seg.selector;
    ws.Attributes =
        static_cast<UINT16>(
            (seg.type & 0xF)             |
            ((seg.s & 1) << 4)           |
            ((seg.dpl & 3) << 5)         |
            ((seg.present & 1) << 7)     |
            ((seg.avl & 1) << 12)        |
            ((seg.l & 1) << 13)          |
            ((seg.db & 1) << 14)         |
            ((seg.g & 1) << 15)
        );
    return ws;
}

// Helper: convert a WHV_X64_SEGMENT_REGISTER to our X86Segment
static X86Segment from_whv_segment(const WHV_X64_SEGMENT_REGISTER& ws) {
    X86Segment seg{};
    seg.base     = ws.Base;
    seg.limit    = ws.Limit;
    seg.selector = ws.Selector;
    seg.type     = static_cast<uint8_t>(ws.Attributes & 0xF);
    seg.s        = static_cast<uint8_t>((ws.Attributes >> 4) & 1);
    seg.dpl      = static_cast<uint8_t>((ws.Attributes >> 5) & 3);
    seg.present  = static_cast<uint8_t>((ws.Attributes >> 7) & 1);
    seg.avl      = static_cast<uint8_t>((ws.Attributes >> 12) & 1);
    seg.l        = static_cast<uint8_t>((ws.Attributes >> 13) & 1);
    seg.db       = static_cast<uint8_t>((ws.Attributes >> 14) & 1);
    seg.g        = static_cast<uint8_t>((ws.Attributes >> 15) & 1);
    return seg;
}

HalResult<X86Sregs> WhpxVcpu::get_sregs() const {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    // Segment registers (CS, DS, ES, FS, GS, SS, TR, LDTR),
    // descriptor tables (GDTR, IDTR),
    // control registers (CR0, CR2, CR3, CR4),
    // and EFER
    static constexpr WHV_REGISTER_NAME reg_names[] = {
        WHvX64RegisterCs,           // 0
        WHvX64RegisterDs,           // 1
        WHvX64RegisterEs,           // 2
        WHvX64RegisterFs,           // 3
        WHvX64RegisterGs,           // 4
        WHvX64RegisterSs,           // 5
        WHvX64RegisterTr,           // 6
        WHvX64RegisterLdtr,         // 7
        WHvX64RegisterGdtr,         // 8
        WHvX64RegisterIdtr,         // 9
        WHvX64RegisterCr0,          // 10
        WHvX64RegisterCr2,          // 11
        WHvX64RegisterCr3,          // 12
        WHvX64RegisterCr4,          // 13
        WHvX64RegisterEfer,         // 14
    };
    static constexpr UINT32 reg_count =
        static_cast<UINT32>(std::size(reg_names));

    WHV_REGISTER_VALUE values[reg_count]{};
    HRESULT hr = WHvGetVirtualProcessorRegisters(
        partition_, id_, reg_names, reg_count, values);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }

    X86Sregs sregs{};

    // Segment registers are stored as WHV_X64_SEGMENT_REGISTER in the Segment
    // member of WHV_REGISTER_VALUE
    sregs.cs  = from_whv_segment(values[0].Segment);
    sregs.ds  = from_whv_segment(values[1].Segment);
    sregs.es  = from_whv_segment(values[2].Segment);
    sregs.fs  = from_whv_segment(values[3].Segment);
    sregs.gs  = from_whv_segment(values[4].Segment);
    sregs.ss  = from_whv_segment(values[5].Segment);
    sregs.tr  = from_whv_segment(values[6].Segment);
    sregs.ldt = from_whv_segment(values[7].Segment);

    // GDT and IDT are stored as WHV_X64_TABLE_REGISTER in the Table member
    sregs.gdt.base  = values[8].Table.Base;
    sregs.gdt.limit = values[8].Table.Limit;
    sregs.idt.base  = values[9].Table.Base;
    sregs.idt.limit = values[9].Table.Limit;

    // Control registers
    sregs.cr0  = values[10].Reg64;
    sregs.cr2  = values[11].Reg64;
    sregs.cr3  = values[12].Reg64;
    sregs.cr4  = values[13].Reg64;
    sregs.efer = values[14].Reg64;

    return sregs;
}

HalResult<void> WhpxVcpu::set_sregs(const X86Sregs& sregs) {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    static constexpr WHV_REGISTER_NAME reg_names[] = {
        WHvX64RegisterCs,           // 0
        WHvX64RegisterDs,           // 1
        WHvX64RegisterEs,           // 2
        WHvX64RegisterFs,           // 3
        WHvX64RegisterGs,           // 4
        WHvX64RegisterSs,           // 5
        WHvX64RegisterTr,           // 6
        WHvX64RegisterLdtr,         // 7
        WHvX64RegisterGdtr,         // 8
        WHvX64RegisterIdtr,         // 9
        WHvX64RegisterCr0,          // 10
        WHvX64RegisterCr2,          // 11
        WHvX64RegisterCr3,          // 12
        WHvX64RegisterCr4,          // 13
        WHvX64RegisterEfer,         // 14
    };
    static constexpr UINT32 reg_count =
        static_cast<UINT32>(std::size(reg_names));

    WHV_REGISTER_VALUE values[reg_count]{};

    // Segment registers
    values[0].Segment = to_whv_segment(sregs.cs);
    values[1].Segment = to_whv_segment(sregs.ds);
    values[2].Segment = to_whv_segment(sregs.es);
    values[3].Segment = to_whv_segment(sregs.fs);
    values[4].Segment = to_whv_segment(sregs.gs);
    values[5].Segment = to_whv_segment(sregs.ss);
    values[6].Segment = to_whv_segment(sregs.tr);
    values[7].Segment = to_whv_segment(sregs.ldt);

    // Descriptor tables
    values[8].Table.Base  = sregs.gdt.base;
    values[8].Table.Limit = sregs.gdt.limit;
    values[9].Table.Base  = sregs.idt.base;
    values[9].Table.Limit = sregs.idt.limit;

    // Control registers
    values[10].Reg64 = sregs.cr0;
    values[11].Reg64 = sregs.cr2;
    values[12].Reg64 = sregs.cr3;
    values[13].Reg64 = sregs.cr4;
    values[14].Reg64 = sregs.efer;

    HRESULT hr = WHvSetVirtualProcessorRegisters(
        partition_, id_, reg_names, reg_count, values);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<void> WhpxVcpu::inject_interrupt(uint32_t irq) {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    // Use the pending interrupt register to inject into this specific vCPU.
    // WHvRegisterPendingInterruption controls the interrupt injection state.
    WHV_REGISTER_NAME reg_name = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE reg_value{};

    // WHV_X64_PENDING_INTERRUPTION_REGISTER fields:
    //   InterruptionPending = 1
    //   InterruptionType = WHvX64PendingInterrupt (0 = external interrupt)
    //   InterruptionVector = irq vector
    reg_value.PendingInterruption.InterruptionPending = 1;
    reg_value.PendingInterruption.InterruptionType = 0; // External interrupt
    reg_value.PendingInterruption.InterruptionVector = irq;
    reg_value.PendingInterruption.DeliverErrorCode = 0;
    reg_value.PendingInterruption.ErrorCode = 0;

    HRESULT hr = WHvSetVirtualProcessorRegisters(
        partition_, id_, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

HalResult<uint64_t> WhpxVcpu::get_msr(uint32_t index) const {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    // WHPX exposes MSRs through its register namespace. Well-known MSRs have
    // dedicated WHV_REGISTER_NAME values. For other MSRs we use the Tsc or
    // fall through to the known set.
    //
    // Map common MSR indices to WHV_REGISTER_NAME. The WHPX API does not
    // provide a generic "read arbitrary MSR by index" call -- each MSR must
    // be mapped to a WHV_REGISTER_NAME constant.
    WHV_REGISTER_NAME reg_name;

    switch (index) {
        case 0x00000010: // IA32_TIME_STAMP_COUNTER
            reg_name = WHvX64RegisterTsc;
            break;
        case 0x00000174: // IA32_SYSENTER_CS
            reg_name = WHvX64RegisterSysenterCs;
            break;
        case 0x00000175: // IA32_SYSENTER_ESP
            reg_name = WHvX64RegisterSysenterEsp;
            break;
        case 0x00000176: // IA32_SYSENTER_EIP
            reg_name = WHvX64RegisterSysenterEip;
            break;
        case 0x000001A0: // IA32_MISC_ENABLE
            // No direct WHV register for MISC_ENABLE; unsupported
            return std::unexpected(HalError::NotSupported);
        case 0xC0000080: // IA32_EFER
            reg_name = WHvX64RegisterEfer;
            break;
        case 0xC0000081: // IA32_STAR
            reg_name = WHvX64RegisterStar;
            break;
        case 0xC0000082: // IA32_LSTAR
            reg_name = WHvX64RegisterLstar;
            break;
        case 0xC0000083: // IA32_CSTAR
            reg_name = WHvX64RegisterCstar;
            break;
        case 0xC0000084: // IA32_FMASK
            reg_name = WHvX64RegisterSfmask;
            break;
        case 0xC0000100: // IA32_FS_BASE
            reg_name = WHvX64RegisterFsBase;
            break;
        case 0xC0000101: // IA32_GS_BASE
            reg_name = WHvX64RegisterGsBase;
            break;
        case 0xC0000102: // IA32_KERNEL_GS_BASE
            reg_name = WHvX64RegisterKernelGsBase;
            break;
        case 0x00000277: // IA32_PAT
            reg_name = WHvX64RegisterPat;
            break;
        case 0xC0000103: // IA32_TSC_AUX
            reg_name = WHvX64RegisterTscAux;
            break;
        case 0x00000802: // IA32_X2APIC_APICID (APIC base)
        case 0x0000001B: // IA32_APIC_BASE
            reg_name = WHvX64RegisterApicBase;
            break;
        default:
            return std::unexpected(HalError::NotSupported);
    }

    WHV_REGISTER_VALUE value{};
    HRESULT hr = WHvGetVirtualProcessorRegisters(
        partition_, id_, &reg_name, 1, &value);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }
    return value.Reg64;
}

HalResult<void> WhpxVcpu::set_msr(uint32_t index, uint64_t value) {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    WHV_REGISTER_NAME reg_name;

    switch (index) {
        case 0x00000010: reg_name = WHvX64RegisterTsc;          break;
        case 0x00000174: reg_name = WHvX64RegisterSysenterCs;   break;
        case 0x00000175: reg_name = WHvX64RegisterSysenterEsp;  break;
        case 0x00000176: reg_name = WHvX64RegisterSysenterEip;  break;
        case 0xC0000080: reg_name = WHvX64RegisterEfer;         break;
        case 0xC0000081: reg_name = WHvX64RegisterStar;         break;
        case 0xC0000082: reg_name = WHvX64RegisterLstar;        break;
        case 0xC0000083: reg_name = WHvX64RegisterCstar;        break;
        case 0xC0000084: reg_name = WHvX64RegisterSfmask;       break;
        case 0xC0000100: reg_name = WHvX64RegisterFsBase;       break;
        case 0xC0000101: reg_name = WHvX64RegisterGsBase;       break;
        case 0xC0000102: reg_name = WHvX64RegisterKernelGsBase; break;
        case 0x00000277: reg_name = WHvX64RegisterPat;          break;
        case 0xC0000103: reg_name = WHvX64RegisterTscAux;       break;
        case 0x0000001B: reg_name = WHvX64RegisterApicBase;     break;
        default:
            return std::unexpected(HalError::NotSupported);
    }

    WHV_REGISTER_VALUE reg_val{};
    reg_val.Reg64 = value;

    HRESULT hr = WHvSetVirtualProcessorRegisters(
        partition_, id_, &reg_name, 1, &reg_val);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

// ============================================================================
// WhpxMemoryManager
// ============================================================================

WhpxMemoryManager::WhpxMemoryManager(WHV_PARTITION_HANDLE partition)
    : partition_(partition) {}

HalResult<void> WhpxMemoryManager::map_region(const MemoryRegion& region) {
    WHV_MAP_GPA_RANGE_FLAGS flags = WHvMapGpaRangeFlagRead;
    if (!region.readonly) {
        flags |= WHvMapGpaRangeFlagWrite;
    }
    flags |= WHvMapGpaRangeFlagExecute;

    HRESULT hr = WHvMapGpaRange(
        partition_,
        reinterpret_cast<void*>(region.userspace_addr),
        region.guest_phys_addr,
        region.size,
        flags);
    if (FAILED(hr)) {
        return std::unexpected(HalError::MemoryMappingFailed);
    }

    // Track the region -- update in place if slot exists, else append
    for (auto& r : regions_) {
        if (r.slot == region.slot) {
            r = region;
            return {};
        }
    }
    regions_.push_back(region);
    return {};
}

HalResult<void> WhpxMemoryManager::unmap_region(uint32_t slot) {
    auto it = std::find_if(regions_.begin(), regions_.end(),
        [slot](const MemoryRegion& r) { return r.slot == slot; });

    if (it == regions_.end()) {
        return std::unexpected(HalError::InvalidParameter);
    }

    HRESULT hr = WHvUnmapGpaRange(partition_, it->guest_phys_addr, it->size);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }

    regions_.erase(it);
    return {};
}

HalResult<HVA> WhpxMemoryManager::gpa_to_hva(GPA gpa) const {
    for (const auto& r : regions_) {
        if (gpa >= r.guest_phys_addr && gpa < r.guest_phys_addr + r.size) {
            return r.userspace_addr + (gpa - r.guest_phys_addr);
        }
    }
    return std::unexpected(HalError::InvalidParameter);
}

std::vector<MemoryRegion> WhpxMemoryManager::get_regions() const {
    return regions_;
}

// ============================================================================
// WhpxHypervisor
// ============================================================================

WhpxHypervisor::WhpxHypervisor() = default;

WhpxHypervisor::~WhpxHypervisor() {
    if (partition_) {
        WHvDeletePartition(partition_);
        partition_ = nullptr;
    }
}

bool WhpxHypervisor::is_available() const {
    WHV_CAPABILITY cap{};
    UINT32 written = 0;
    HRESULT hr = WHvGetCapability(
        WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &written);
    return SUCCEEDED(hr) && cap.HypervisorPresent;
}

HalResult<void> WhpxHypervisor::initialize() {
    if (!is_available()) {
        return std::unexpected(HalError::NotSupported);
    }

    // Verify extended VM exits capability for memory/IO intercepts
    WHV_CAPABILITY cap{};
    UINT32 written = 0;
    HRESULT hr = WHvGetCapability(
        WHvCapabilityCodeFeatures, &cap, sizeof(cap), &written);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }

    // The Features capability tells us what the platform supports.
    // We proceed even if some optional features are missing -- the
    // partition property setup in create_vm will request what we need.

    return {};
}

HalResult<void> WhpxHypervisor::create_vm(const VmConfig& config) {
    config_ = config;

    // Create the partition (WHPX equivalent of a VM)
    HRESULT hr = WHvCreatePartition(&partition_);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }

    // Set processor count
    WHV_PARTITION_PROPERTY prop{};
    prop.ProcessorCount = config.num_vcpus;
    hr = WHvSetPartitionProperty(
        partition_,
        WHvPartitionPropertyCodeProcessorCount,
        &prop, sizeof(prop.ProcessorCount));
    if (FAILED(hr)) {
        WHvDeletePartition(partition_);
        partition_ = nullptr;
        return std::unexpected(HalError::InternalError);
    }

    // Enable extended VM exits for X64 MSR access
    // This allows us to intercept specific MSR reads/writes if needed.
    WHV_PARTITION_PROPERTY exit_prop{};
    exit_prop.ExtendedVmExits.X64MsrExit = 1;
    exit_prop.ExtendedVmExits.ExceptionExit = 0;
    hr = WHvSetPartitionProperty(
        partition_,
        WHvPartitionPropertyCodeExtendedVmExits,
        &exit_prop, sizeof(exit_prop.ExtendedVmExits));
    // MSR exit is optional; don't fail if the platform doesn't support it
    // (the partition will still work for basic MMIO/IO).

    // Configure local APIC emulation mode if irqchip is requested
    if (config.enable_irqchip) {
        WHV_PARTITION_PROPERTY apic_prop{};
        apic_prop.LocalApicEmulationMode =
            WHvX64LocalApicEmulationModeXApic;
        hr = WHvSetPartitionProperty(
            partition_,
            WHvPartitionPropertyCodeLocalApicEmulationMode,
            &apic_prop, sizeof(apic_prop.LocalApicEmulationMode));
        // Non-fatal if APIC emulation mode setting fails --
        // WHPX provides default APIC behavior.
    }

    // Finalize partition setup -- must be called before creating vCPUs
    hr = WHvSetupPartition(partition_);
    if (FAILED(hr)) {
        WHvDeletePartition(partition_);
        partition_ = nullptr;
        return std::unexpected(HalError::InternalError);
    }

    // Create the memory manager for this partition
    mem_mgr_ = std::make_unique<WhpxMemoryManager>(partition_);

    return {};
}

HalResult<std::unique_ptr<IVcpu>> WhpxHypervisor::create_vcpu(VcpuId id) {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    HRESULT hr = WHvCreateVirtualProcessor(partition_, id, 0);
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }

    auto vcpu = std::make_unique<WhpxVcpu>(partition_, id);
    return vcpu;
}

IMemoryManager& WhpxHypervisor::memory_manager() {
    return *mem_mgr_;
}

HalResult<void> WhpxHypervisor::create_irqchip() {
    // WHPX provides an in-kernel local APIC by default when the partition
    // is set up. The LocalApicEmulationMode property was already configured
    // in create_vm() if enable_irqchip was true. No additional setup needed.
    //
    // Unlike KVM, WHPX does not require explicit IRQCHIP/PIT creation calls
    // -- the hypervisor platform handles timer and APIC emulation internally.
    return {};
}

HalResult<void> WhpxHypervisor::set_irq_line(uint32_t irq, bool level) {
    if (!partition_) {
        return std::unexpected(HalError::NotInitialized);
    }

    WHV_INTERRUPT_CONTROL ctrl{};
    ctrl.Type = WHvX64InterruptTypeFixed;
    ctrl.DestinationMode = WHvX64InterruptDestinationModePhysical;
    ctrl.TriggerMode = level
        ? WHvX64InterruptTriggerModeLevel
        : WHvX64InterruptTriggerModeEdge;
    ctrl.Vector = irq;
    ctrl.Destination = 0; // BSP (bootstrap processor)

    HRESULT hr = WHvRequestInterrupt(partition_, &ctrl, sizeof(ctrl));
    if (FAILED(hr)) {
        return std::unexpected(HalError::InternalError);
    }
    return {};
}

int WhpxHypervisor::api_version() const {
    // WHPX does not expose a versioned API like KVM. We return 1 as a
    // sentinel value indicating the Windows Hypervisor Platform API.
    return 1;
}

} // namespace rex::hal

#endif // _WIN32
