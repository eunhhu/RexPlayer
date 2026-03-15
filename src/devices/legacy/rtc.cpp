#include "rtc.h"
#include <ctime>

namespace rex::devices {

void Rtc::io_access(rex::hal::IoAccess& access) {
    if (access.port == INDEX_PORT) {
        if (access.is_write) {
            index_ = static_cast<uint8_t>(access.data & 0x7F);
        } else {
            access.data = index_;
        }
    } else if (access.port == DATA_PORT) {
        if (access.is_write) {
            // CMOS writes are ignored (read-only time source)
        } else {
            access.data = read_cmos(index_);
        }
    }
}

void Rtc::mmio_access(rex::hal::MmioAccess& /*access*/) {}

uint8_t Rtc::read_cmos(uint8_t reg) const {
    time_t now = time(nullptr);
    struct tm* t = gmtime(&now);

    // Convert to BCD
    auto to_bcd = [](int val) -> uint8_t {
        return static_cast<uint8_t>(((val / 10) << 4) | (val % 10));
    };

    switch (reg) {
        case 0x00: return to_bcd(t->tm_sec);
        case 0x02: return to_bcd(t->tm_min);
        case 0x04: return to_bcd(t->tm_hour);
        case 0x06: return to_bcd(t->tm_wday + 1);
        case 0x07: return to_bcd(t->tm_mday);
        case 0x08: return to_bcd(t->tm_mon + 1);
        case 0x09: return to_bcd(t->tm_year % 100);
        case 0x0A: return 0x26; // Status Register A: divider + rate
        case 0x0B: return 0x02; // Status Register B: 24h mode, BCD
        case 0x0C: return 0x00; // Status Register C: no interrupts
        case 0x0D: return 0x80; // Status Register D: valid RAM/time
        case 0x32: return to_bcd((t->tm_year + 1900) / 100); // Century
        default:   return 0x00;
    }
}

} // namespace rex::devices
