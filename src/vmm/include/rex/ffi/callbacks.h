#pragma once

#include <cstdint>
#include <cstddef>

namespace rex::ffi {

struct IrqRequest;

/// Callbacks from Rust middleware into C++ VMM
/// These are implemented in the VMM core and called by Rust via cxx bridge.

void inject_irq(IrqRequest req);

/// Read from guest physical memory
/// Returns true on success
bool read_guest_memory(uint64_t gpa, uint8_t* buf, size_t len);

/// Write to guest physical memory
/// Returns true on success
bool write_guest_memory(uint64_t gpa, const uint8_t* buf, size_t len);

} // namespace rex::ffi
