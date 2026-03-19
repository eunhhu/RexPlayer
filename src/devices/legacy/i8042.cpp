#include "i8042.h"

namespace rex::devices {

void I8042::io_access(rex::hal::IoAccess& access) {
    if (access.is_write) {
        switch (access.port) {
            case DATA_PORT:
                // Accept and ignore keyboard commands
                break;
            case STATUS_PORT:
                // Command byte — minimal handling
                break;
            default:
                break;
        }
    } else {
        switch (access.port) {
            case DATA_PORT:
                access.data = output_byte_;
                break;
            case STATUS_PORT:
                // Status: output buffer empty, input buffer empty
                access.data = status_;
                break;
            default:
                access.data = 0;
                break;
        }
    }
}

void I8042::mmio_access(rex::hal::MmioAccess& /*access*/) {
    // i8042 uses port I/O only
}

} // namespace rex::devices
