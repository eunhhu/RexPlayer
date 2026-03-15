#pragma once

#include "rex/hal/types.h"
#include <string>

namespace rex::vmm {

class Vm;

/// Save and restore VM state (vCPU registers, RAM, device state)
class SnapshotManager {
public:
    /// Save the entire VM state to a file
    static rex::hal::HalResult<void> save(const Vm& vm, const std::string& path);

    /// Restore a VM from a saved snapshot
    static rex::hal::HalResult<void> restore(Vm& vm, const std::string& path);
};

} // namespace rex::vmm
