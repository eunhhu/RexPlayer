#pragma once

#if defined(__APPLE__) && defined(__aarch64__)

#include "rex/hal/types.h"
#include <cstdint>

namespace rex::hal {

/// ARM64 general-purpose registers (X0-X30, PC, SP, CPSR)
struct Arm64Regs {
    uint64_t x[31];     // X0 - X30
    uint64_t pc;        // Program Counter
    uint64_t sp;        // Stack Pointer (SP_EL1)
    uint64_t cpsr;      // Current Program Status Register (PSTATE)
};

/// ARM64 system registers for EL1 configuration
struct Arm64SysRegs {
    uint64_t sctlr_el1;     // System Control Register
    uint64_t tcr_el1;       // Translation Control Register
    uint64_t ttbr0_el1;     // Translation Table Base Register 0
    uint64_t ttbr1_el1;     // Translation Table Base Register 1
    uint64_t mair_el1;      // Memory Attribute Indirection Register
    uint64_t vbar_el1;      // Vector Base Address Register
    uint64_t esr_el1;       // Exception Syndrome Register
    uint64_t far_el1;       // Fault Address Register
    uint64_t elr_el1;       // Exception Link Register
    uint64_t spsr_el1;      // Saved Program Status Register
    uint64_t sp_el0;        // Stack Pointer EL0
    uint64_t tpidr_el0;     // Thread Pointer/ID Register EL0
    uint64_t tpidr_el1;     // Thread Pointer/ID Register EL1
    uint64_t tpidrro_el0;   // Thread Pointer/ID Register (Read-Only) EL0
    uint64_t midr_el1;      // Main ID Register
    uint64_t mpidr_el1;     // Multiprocessor Affinity Register
    uint64_t par_el1;       // Physical Address Register
    uint64_t cntv_ctl_el0;  // Counter-timer Virtual Timer Control Register
    uint64_t cntv_cval_el0; // Counter-timer Virtual Timer CompareValue Register
};

/// ARM64 SIMD/FP registers (V0-V31)
struct Arm64FpRegs {
    __uint128_t v[32];  // V0 - V31 (128-bit SIMD registers)
    uint32_t fpcr;      // Floating-Point Control Register
    uint32_t fpsr;      // Floating-Point Status Register
};

/// ARM64 vCPU exit reasons (mapped from HVF exit info)
struct Arm64VcpuExit {
    enum class Reason : uint32_t {
        Exception,       // Synchronous exception (data abort → MMIO, HVC, SMC, WFI, etc.)
        IrqPending,      // IRQ window open
        VtimerActivated, // Virtual timer fired
        Canceled,        // vCPU execution was canceled
        Unknown,
        InternalError,
    };

    Reason reason;

    /// Exception-specific fields (valid when reason == Exception)
    struct ExceptionInfo {
        uint64_t syndrome;   // ESR_EL2 value
        uint64_t va;         // Virtual address (for address-related exceptions)
        uint64_t pa;         // IPA / physical address (for stage-2 faults)
        bool is_write;
        uint8_t access_size; // For data aborts: 1, 2, 4, or 8
    };

    /// Exception class extracted from ESR_EL2
    enum class ExceptionClass : uint8_t {
        DataAbortLowerEL  = 0x24,  // EC = 0b100100
        DataAbortSameEL   = 0x25,  // EC = 0b100101
        InstrAbortLowerEL = 0x20,  // EC = 0b100000
        InstrAbortSameEL  = 0x21,  // EC = 0b100001
        HvcAarch64        = 0x16,  // EC = 0b010110
        SmcAarch64        = 0x17,  // EC = 0b010111
        WfxTrap           = 0x01,  // EC = 0b000001 (WFI/WFE)
        SysRegTrap        = 0x18,  // EC = 0b011000 (MSR/MRS trap)
    };

    ExceptionInfo exception;

    /// Extract the exception class (EC) from the syndrome (ESR_EL2 bits [31:26])
    ExceptionClass exception_class() const {
        return static_cast<ExceptionClass>((exception.syndrome >> 26) & 0x3F);
    }

    /// Check if the data fault status code indicates a translation fault (MMIO)
    bool is_translation_fault() const {
        uint32_t dfsc = exception.syndrome & 0x3F;
        // Translation faults: DFSC 0b0001xx (levels 0-3)
        return (dfsc & 0x3C) == 0x04;
    }
};

/// Decode ISS (Instruction Specific Syndrome) for data aborts
struct DataAbortIss {
    bool isv;        // Instruction Syndrome Valid
    uint8_t sas;     // Syndrome Access Size (0=byte, 1=half, 2=word, 3=dword)
    bool sse;        // Syndrome Sign Extend
    uint8_t srt;     // Syndrome Register Transfer (Xt register index)
    bool sf;         // Sixty-Four bit register
    bool ar;         // Acquire/Release
    bool wnr;        // Write Not Read
    uint8_t dfsc;    // Data Fault Status Code

    /// Construct from ESR_EL2 syndrome value
    static DataAbortIss decode(uint64_t syndrome) {
        DataAbortIss iss{};
        iss.isv  = (syndrome >> 24) & 1;
        iss.sas  = (syndrome >> 22) & 3;
        iss.sse  = (syndrome >> 21) & 1;
        iss.srt  = (syndrome >> 16) & 0x1F;
        iss.sf   = (syndrome >> 15) & 1;
        iss.ar   = (syndrome >> 14) & 1;
        iss.wnr  = (syndrome >> 6) & 1;
        iss.dfsc = syndrome & 0x3F;
        return iss;
    }

    /// Get the access size in bytes
    uint8_t access_size_bytes() const {
        return static_cast<uint8_t>(1u << sas);
    }
};

} // namespace rex::hal

#endif // defined(__APPLE__) && defined(__aarch64__)
