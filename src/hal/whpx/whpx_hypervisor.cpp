#ifdef _WIN32

#include "whpx_hypervisor.h"

#include <WinHvPlatform.h>
#include <WinHvEmulation.h>

namespace rex::hal {

// ============================================================================
// WhpxVcpu
// ============================================================================

WhpxVcpu::WhpxVcpu(VcpuId id) : id_(id) {}
WhpxVcpu::~WhpxVcpu() = default;

HalResult<VcpuExit> WhpxVcpu::run() {
    // TODO: WHvRunVirtualProcessor implementation
    return std::unexpected(HalError::NotSupported);
}

HalResult<X86Regs> WhpxVcpu::get_regs() const {
    return std::unexpected(HalError::NotSupported);
}

HalResult<void> WhpxVcpu::set_regs(const X86Regs&) {
    return std::unexpected(HalError::NotSupported);
}

HalResult<X86Sregs> WhpxVcpu::get_sregs() const {
    return std::unexpected(HalError::NotSupported);
}

HalResult<void> WhpxVcpu::set_sregs(const X86Sregs&) {
    return std::unexpected(HalError::NotSupported);
}

HalResult<void> WhpxVcpu::inject_interrupt(uint32_t) {
    return std::unexpected(HalError::NotSupported);
}

HalResult<uint64_t> WhpxVcpu::get_msr(uint32_t) const {
    return std::unexpected(HalError::NotSupported);
}

HalResult<void> WhpxVcpu::set_msr(uint32_t, uint64_t) {
    return std::unexpected(HalError::NotSupported);
}

// ============================================================================
// WhpxMemoryManager
// ============================================================================

WhpxMemoryManager::WhpxMemoryManager(void* partition) : partition_(partition) {}

HalResult<void> WhpxMemoryManager::map_region(const MemoryRegion& region) {
    HRESULT hr = WHvMapGpaRange(
        static_cast<WHV_PARTITION_HANDLE>(partition_),
        reinterpret_cast<void*>(region.userspace_addr),
        region.guest_phys_addr,
        region.size,
        region.readonly
            ? WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute
            : WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute
    );

    if (FAILED(hr)) {
        return std::unexpected(HalError::MemoryMappingFailed);
    }

    for (auto& r : regions_) {
        if (r.slot == region.slot) { r = region; return {}; }
    }
    regions_.push_back(region);
    return {};
}

HalResult<void> WhpxMemoryManager::unmap_region(uint32_t slot) {
    auto it = std::find_if(regions_.begin(), regions_.end(),
        [slot](const MemoryRegion& r) { return r.slot == slot; });

    if (it == regions_.end()) return std::unexpected(HalError::InvalidParameter);

    WHvUnmapGpaRange(
        static_cast<WHV_PARTITION_HANDLE>(partition_),
        it->guest_phys_addr, it->size
    );
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

std::vector<MemoryRegion> WhpxMemoryManager::get_regions() const { return regions_; }

// ============================================================================
// WhpxHypervisor
// ============================================================================

WhpxHypervisor::WhpxHypervisor() = default;

WhpxHypervisor::~WhpxHypervisor() {
    if (partition_) {
        WHvDeletePartition(static_cast<WHV_PARTITION_HANDLE>(partition_));
    }
}

bool WhpxHypervisor::is_available() const {
    WHV_CAPABILITY cap{};
    uint32_t size = 0;
    HRESULT hr = WHvGetCapability(
        WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &size);
    return SUCCEEDED(hr) && cap.HypervisorPresent;
}

HalResult<void> WhpxHypervisor::initialize() {
    if (!is_available()) return std::unexpected(HalError::NotSupported);
    return {};
}

HalResult<void> WhpxHypervisor::create_vm(const VmConfig& config) {
    config_ = config;

    WHV_PARTITION_HANDLE part;
    HRESULT hr = WHvCreatePartition(&part);
    if (FAILED(hr)) return std::unexpected(HalError::InternalError);
    partition_ = part;

    // Set processor count
    WHV_PARTITION_PROPERTY prop{};
    prop.ProcessorCount = config.num_vcpus;
    hr = WHvSetPartitionProperty(part, WHvPartitionPropertyCodeProcessorCount,
                                  &prop, sizeof(prop));
    if (FAILED(hr)) return std::unexpected(HalError::InternalError);

    hr = WHvSetupPartition(part);
    if (FAILED(hr)) return std::unexpected(HalError::InternalError);

    mem_mgr_ = std::make_unique<WhpxMemoryManager>(partition_);
    return {};
}

HalResult<std::unique_ptr<IVcpu>> WhpxHypervisor::create_vcpu(VcpuId id) {
    if (!partition_) return std::unexpected(HalError::NotInitialized);

    HRESULT hr = WHvCreateVirtualProcessor(
        static_cast<WHV_PARTITION_HANDLE>(partition_), id, 0);
    if (FAILED(hr)) return std::unexpected(HalError::InternalError);

    auto vcpu = std::make_unique<WhpxVcpu>(id);
    vcpu->partition_ = partition_;
    return vcpu;
}

IMemoryManager& WhpxHypervisor::memory_manager() { return *mem_mgr_; }

HalResult<void> WhpxHypervisor::create_irqchip() {
    // WHPX provides in-kernel APIC by default
    return {};
}

HalResult<void> WhpxHypervisor::set_irq_line(uint32_t irq, bool level) {
    WHV_INTERRUPT_CONTROL ctrl{};
    ctrl.Type = WHvX64InterruptTypeFixed;
    ctrl.DestinationMode = WHvX64InterruptDestinationModePhysical;
    ctrl.TriggerMode = level
        ? WHvX64InterruptTriggerModeLevel
        : WHvX64InterruptTriggerModeEdge;
    ctrl.Vector = irq;
    ctrl.Destination = 0;

    HRESULT hr = WHvRequestInterrupt(
        static_cast<WHV_PARTITION_HANDLE>(partition_), &ctrl, sizeof(ctrl));
    if (FAILED(hr)) return std::unexpected(HalError::InternalError);
    return {};
}

int WhpxHypervisor::api_version() const { return 1; }

} // namespace rex::hal

#endif // _WIN32
