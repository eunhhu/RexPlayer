#pragma once

#include <cstdint>
#include <cstddef>

namespace rex::ffi {

struct IrqRequest;
struct GpuResourceCreate;
struct GpuScanout;
struct GpuTransfer;

// ============================================================================
// VMM callbacks (Rust → C++)
// ============================================================================

/// Inject an interrupt into the VM
void inject_irq(IrqRequest req);

/// Read from guest physical memory
bool read_guest_memory(uint64_t gpa, uint8_t* buf, size_t len);

/// Write to guest physical memory
bool write_guest_memory(uint64_t gpa, const uint8_t* buf, size_t len);

// ============================================================================
// GPU callbacks (Rust virtio-gpu → C++ GpuBridge)
// ============================================================================

/// Create a 2D GPU resource on the host renderer
bool gpu_resource_create_2d(const GpuResourceCreate& req);

/// Destroy a GPU resource
void gpu_resource_unref(uint32_t resource_id);

/// Set scanout (which resource is displayed)
bool gpu_set_scanout(const GpuScanout& req);

/// Flush a resource region to the display
bool gpu_resource_flush(const GpuTransfer& req);

} // namespace rex::ffi
