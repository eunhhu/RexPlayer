#include "pci_host.h"

namespace rex::devices {

void PciHost::io_access(rex::hal::IoAccess& access) {
    if (access.port >= CONFIG_ADDR && access.port < CONFIG_ADDR + 4) {
        // Configuration Address Register (0xCF8)
        if (access.is_write) {
            config_address_ = access.data;
        } else {
            access.data = config_address_;
        }
    } else if (access.port >= CONFIG_DATA && access.port < CONFIG_DATA + 4) {
        // Configuration Data Register (0xCFC-0xCFF)
        if (!(config_address_ & 0x80000000)) {
            // Enable bit not set
            if (!access.is_write) access.data = 0xFFFFFFFF;
            return;
        }

        uint8_t bus    = (config_address_ >> 16) & 0xFF;
        uint8_t device = (config_address_ >> 11) & 0x1F;
        uint8_t func   = (config_address_ >> 8) & 0x07;
        uint8_t offset = (config_address_ & 0xFC) + (access.port - CONFIG_DATA);

        auto* pci_dev = find_device(bus, device, func);
        if (!pci_dev) {
            if (!access.is_write) access.data = 0xFFFFFFFF;
            return;
        }

        if (access.is_write) {
            pci_dev->config_write(offset, access.data, access.size);
        } else {
            access.data = pci_dev->config_read(offset, access.size);
        }
    }
}

void PciHost::mmio_access(rex::hal::MmioAccess& /*access*/) {}

void PciHost::add_device(uint8_t bus, uint8_t device, uint8_t function,
                         std::shared_ptr<IPciDevice> dev) {
    devices_.push_back({bus, device, function, std::move(dev)});
}

IPciDevice* PciHost::find_device(uint8_t bus, uint8_t device, uint8_t function) {
    for (auto& slot : devices_) {
        if (slot.bus == bus && slot.device == device && slot.function == function) {
            return slot.dev.get();
        }
    }
    return nullptr;
}

} // namespace rex::devices
