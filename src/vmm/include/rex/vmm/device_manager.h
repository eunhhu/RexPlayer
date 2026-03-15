#pragma once

#include "rex/hal/types.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace rex::vmm {

/// Interface for an emulated device
class IDevice {
public:
    virtual ~IDevice() = default;

    /// Device name for debugging
    virtual std::string name() const = 0;

    /// Handle an I/O port read/write
    virtual void io_access(rex::hal::IoAccess& access) = 0;

    /// Handle an MMIO read/write
    virtual void mmio_access(rex::hal::MmioAccess& access) = 0;
};

/// Manages device registration and I/O dispatch
class DeviceManager {
public:
    /// Register a device for a range of I/O ports
    void register_io(uint16_t base, uint16_t count, std::shared_ptr<IDevice> device);

    /// Register a device for an MMIO range
    void register_mmio(rex::hal::GPA base, rex::hal::MemSize size, std::shared_ptr<IDevice> device);

    /// Dispatch an I/O port access to the appropriate device
    /// Returns true if a device handled the access
    bool dispatch_io(rex::hal::IoAccess& access);

    /// Dispatch an MMIO access to the appropriate device
    /// Returns true if a device handled the access
    bool dispatch_mmio(rex::hal::MmioAccess& access);

private:
    struct IoRange {
        uint16_t base;
        uint16_t count;
        std::shared_ptr<IDevice> device;
    };

    struct MmioRange {
        rex::hal::GPA base;
        rex::hal::MemSize size;
        std::shared_ptr<IDevice> device;
    };

    std::vector<IoRange> io_ranges_;
    std::vector<MmioRange> mmio_ranges_;
};

} // namespace rex::vmm
