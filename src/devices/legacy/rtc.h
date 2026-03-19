#pragma once

#include "rex/vmm/device_manager.h"
#include <cstdint>

namespace rex::devices {

/// MC146818 RTC (Real-Time Clock) emulation
///
/// Provides time information to the guest via CMOS I/O ports 0x70-0x71.
class Rtc : public rex::vmm::IDevice {
public:
    std::string name() const override { return "RTC"; }
    void io_access(rex::hal::IoAccess& access) override;
    void mmio_access(rex::hal::MmioAccess& access) override;

    static constexpr uint16_t INDEX_PORT = 0x70;
    static constexpr uint16_t DATA_PORT  = 0x71;
    static constexpr uint16_t PORT_COUNT = 2;

private:
    uint8_t read_cmos(uint8_t reg) const;
    uint8_t index_ = 0;
};

} // namespace rex::devices
