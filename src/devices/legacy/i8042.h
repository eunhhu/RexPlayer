#pragma once

#include "rex/vmm/device_manager.h"
#include <cstdint>

namespace rex::devices {

/// i8042 PS/2 keyboard/mouse controller stub
///
/// Minimal implementation to satisfy early boot probing.
/// Real input goes through virtio-input.
class I8042 : public rex::vmm::IDevice {
public:
    std::string name() const override { return "i8042"; }
    void io_access(rex::hal::IoAccess& access) override;
    void mmio_access(rex::hal::MmioAccess& access) override;

    static constexpr uint16_t DATA_PORT   = 0x60;
    static constexpr uint16_t STATUS_PORT = 0x64;
    static constexpr uint16_t PORT_COUNT  = 5; // 0x60-0x64

private:
    uint8_t status_ = 0x00;
    uint8_t output_byte_ = 0x00;
};

} // namespace rex::devices
