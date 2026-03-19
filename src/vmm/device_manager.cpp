#include "rex/vmm/device_manager.h"

namespace rex::vmm {

void DeviceManager::register_io(uint16_t base, uint16_t count, std::shared_ptr<IDevice> device) {
    io_ranges_.push_back({base, count, std::move(device)});
}

void DeviceManager::register_mmio(rex::hal::GPA base, rex::hal::MemSize size, std::shared_ptr<IDevice> device) {
    mmio_ranges_.push_back({base, size, std::move(device)});
}

bool DeviceManager::dispatch_io(rex::hal::IoAccess& access) {
    for (auto& range : io_ranges_) {
        if (access.port >= range.base && access.port < range.base + range.count) {
            range.device->io_access(access);
            return true;
        }
    }
    return false;
}

bool DeviceManager::dispatch_mmio(rex::hal::MmioAccess& access) {
    for (auto& range : mmio_ranges_) {
        if (access.address >= range.base && access.address < range.base + range.size) {
            range.device->mmio_access(access);
            return true;
        }
    }
    return false;
}

} // namespace rex::vmm
