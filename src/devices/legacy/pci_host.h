#pragma once

#include "rex/vmm/device_manager.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::devices {

/// PCI device configuration space (256 bytes per function)
struct PciDeviceConfig {
    uint16_t vendor_id = 0xFFFF;
    uint16_t device_id = 0xFFFF;
    uint16_t command = 0;
    uint16_t status = 0;
    uint8_t  revision_id = 0;
    uint8_t  prog_if = 0;
    uint8_t  subclass = 0;
    uint8_t  class_code = 0;
    uint8_t  header_type = 0;
    uint32_t bar[6] = {};
    uint16_t subsystem_vendor_id = 0;
    uint16_t subsystem_id = 0;
    uint8_t  interrupt_line = 0;
    uint8_t  interrupt_pin = 0;
    std::array<uint8_t, 256> raw{};
};

/// PCI device interface
class IPciDevice {
public:
    virtual ~IPciDevice() = default;
    virtual PciDeviceConfig& config() = 0;
    virtual void config_write(uint8_t offset, uint32_t value, uint8_t size) = 0;
    virtual uint32_t config_read(uint8_t offset, uint8_t size) = 0;
};

/// PCI Host Bridge — Configuration Space access mechanism (I/O ports 0xCF8-0xCFF)
class PciHost : public rex::vmm::IDevice {
public:
    std::string name() const override { return "PCI Host Bridge"; }
    void io_access(rex::hal::IoAccess& access) override;
    void mmio_access(rex::hal::MmioAccess& access) override;

    /// Add a PCI device at bus:device.function
    void add_device(uint8_t bus, uint8_t device, uint8_t function,
                    std::shared_ptr<IPciDevice> dev);

    static constexpr uint16_t CONFIG_ADDR = 0x0CF8;
    static constexpr uint16_t CONFIG_DATA = 0x0CFC;
    static constexpr uint16_t PORT_COUNT  = 8; // 0xCF8-0xCFF

private:
    struct PciSlot {
        uint8_t bus;
        uint8_t device;
        uint8_t function;
        std::shared_ptr<IPciDevice> dev;
    };

    uint32_t config_address_ = 0;
    std::vector<PciSlot> devices_;

    IPciDevice* find_device(uint8_t bus, uint8_t device, uint8_t function);
};

} // namespace rex::devices
