#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <variant>
#include <expected>
#include <span>

namespace rex::hal {

/// Guest physical address
using GPA = uint64_t;

/// Host virtual address
using HVA = uint64_t;

/// Size type for memory regions
using MemSize = uint64_t;

/// vCPU identifier
using VcpuId = uint32_t;

/// Error codes for HAL operations
enum class HalError : uint32_t {
    Ok = 0,
    NotSupported,
    InvalidParameter,
    OutOfMemory,
    DeviceNotFound,
    AlreadyExists,
    NotInitialized,
    PermissionDenied,
    InternalError,
    VcpuRunFailed,
    MemoryMappingFailed,
};

/// String representation of HalError
inline const char* hal_error_str(HalError err) {
    switch (err) {
        case HalError::Ok:                 return "Ok";
        case HalError::NotSupported:       return "NotSupported";
        case HalError::InvalidParameter:   return "InvalidParameter";
        case HalError::OutOfMemory:        return "OutOfMemory";
        case HalError::DeviceNotFound:     return "DeviceNotFound";
        case HalError::AlreadyExists:      return "AlreadyExists";
        case HalError::NotInitialized:     return "NotInitialized";
        case HalError::PermissionDenied:   return "PermissionDenied";
        case HalError::InternalError:      return "InternalError";
        case HalError::VcpuRunFailed:      return "VcpuRunFailed";
        case HalError::MemoryMappingFailed: return "MemoryMappingFailed";
    }
    return "Unknown";
}

template <typename T>
using HalResult = std::expected<T, HalError>;

/// Memory region descriptor
struct MemoryRegion {
    uint32_t slot;
    GPA guest_phys_addr;
    MemSize size;
    HVA userspace_addr;
    bool readonly;
};

/// I/O port access from guest
struct IoAccess {
    uint16_t port;
    uint8_t size;       // 1, 2, or 4 bytes
    bool is_write;
    uint32_t data;
};

/// MMIO access from guest
struct MmioAccess {
    GPA address;
    uint8_t size;       // 1, 2, 4, or 8 bytes
    bool is_write;
    uint64_t data;
};

/// Reasons a vCPU exits back to the VMM
struct VcpuExit {
    enum class Reason : uint32_t {
        IoAccess,
        MmioAccess,
        Hlt,
        Shutdown,
        IrqWindowOpen,
        Unknown,
        InternalError,
    };

    Reason reason;

    union {
        struct IoAccess io;
        struct MmioAccess mmio;
    };
};

/// x86_64 standard registers
struct X86Regs {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
};

/// x86_64 segment register
struct X86Segment {
    uint64_t base;
    uint32_t limit;
    uint16_t selector;
    uint8_t type;
    uint8_t present;
    uint8_t dpl;
    uint8_t db;
    uint8_t s;
    uint8_t l;
    uint8_t g;
    uint8_t avl;
};

/// x86_64 special registers
struct X86Sregs {
    X86Segment cs, ds, es, fs, gs, ss;
    X86Segment tr, ldt;
    struct {
        uint64_t base;
        uint16_t limit;
    } gdt, idt;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t efer;
};

} // namespace rex::hal
