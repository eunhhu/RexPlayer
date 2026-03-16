#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <variant>

// std::expected is C++23; provide a minimal polyfill for C++20 compilers
#if __has_include(<expected>) && __cplusplus > 202002L
#include <expected>
#else
#include <stdexcept>
namespace std {
template <typename E>
class unexpected {
public:
    explicit unexpected(E e) : error_(static_cast<E&&>(e)) {}
    const E& error() const& { return error_; }
    E& error() & { return error_; }
private:
    E error_;
};

template <typename T, typename E>
class expected {
public:
    expected() : has_value_(true), value_{} {}
    expected(T v) : has_value_(true), value_(static_cast<T&&>(v)) {} // NOLINT
    expected(unexpected<E> u) : has_value_(false), error_(u.error()) {} // NOLINT
    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }
    T& operator*() { return value_; }
    const T& operator*() const { return value_; }
    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }
    const E& error() const { return error_; }
private:
    bool has_value_;
    T value_{};
    E error_{};
};

template <typename E>
class expected<void, E> {
public:
    expected() : has_value_(true) {}
    expected(unexpected<E> u) : has_value_(false), error_(u.error()) {} // NOLINT
    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }
    const E& error() const { return error_; }
private:
    bool has_value_;
    E error_{};
};
} // namespace std
#endif

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
