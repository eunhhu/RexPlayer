#include "uart_16550.h"
#include <cstdio>

namespace rex::devices {

Uart16550::Uart16550(OutputCallback on_output)
    : on_output_(std::move(on_output)) {}

void Uart16550::io_access(rex::hal::IoAccess& access) {
    uint16_t offset = access.port - COM1_BASE;

    if (access.is_write) {
        handle_write(offset, access);
    } else {
        handle_read(offset, access);
    }
}

void Uart16550::mmio_access(rex::hal::MmioAccess& /*access*/) {
    // UART uses port I/O, not MMIO
}

void Uart16550::handle_read(uint16_t offset, rex::hal::IoAccess& access) {
    std::lock_guard lock(mutex_);

    switch (offset) {
        case RBR_THR: {
            if (dlab_) {
                access.data = 0x01; // Divisor latch low (baud rate, ignored)
            } else {
                // Read from receive buffer
                if (!rx_fifo_.empty()) {
                    access.data = rx_fifo_.front();
                    rx_fifo_.pop();
                } else {
                    access.data = 0;
                }
            }
            break;
        }
        case IER:
            access.data = dlab_ ? 0x00 : ier_; // DLM when DLAB=1
            break;
        case IIR_FCR:
            // Interrupt Identification Register
            // Bit 0 = 1 means no interrupt pending
            access.data = 0x01;
            if (fcr_ & 1) access.data |= 0xC0; // FIFOs enabled
            break;
        case LCR:
            access.data = lcr_;
            break;
        case MCR:
            access.data = mcr_;
            break;
        case LSR: {
            uint8_t lsr = LSR_THRE | LSR_TEMT; // Always ready to transmit
            if (!rx_fifo_.empty()) {
                lsr |= LSR_DR;
            }
            access.data = lsr;
            break;
        }
        case MSR:
            access.data = 0xB0; // DCD, DSR, CTS asserted
            break;
        case SCR:
            access.data = scr_;
            break;
        default:
            access.data = 0;
            break;
    }
}

void Uart16550::handle_write(uint16_t offset, rex::hal::IoAccess& access) {
    std::lock_guard lock(mutex_);
    uint8_t val = static_cast<uint8_t>(access.data & 0xFF);

    switch (offset) {
        case RBR_THR:
            if (dlab_) {
                // Divisor latch low — ignore (baud rate not emulated)
            } else {
                // Transmit character
                if (on_output_) {
                    on_output_(static_cast<char>(val));
                } else {
                    putchar(val);
                    fflush(stdout);
                }
            }
            break;
        case IER:
            if (dlab_) {
                // Divisor latch high — ignore
            } else {
                ier_ = val;
            }
            break;
        case IIR_FCR:
            fcr_ = val;
            if (val & 0x02) {
                // Clear receive FIFO
                while (!rx_fifo_.empty()) rx_fifo_.pop();
            }
            break;
        case LCR:
            lcr_ = val;
            dlab_ = (val & 0x80) != 0;
            break;
        case MCR:
            mcr_ = val;
            break;
        case SCR:
            scr_ = val;
            break;
        default:
            break;
    }
}

void Uart16550::inject_input(char c) {
    std::lock_guard lock(mutex_);
    rx_fifo_.push(static_cast<uint8_t>(c));
}

} // namespace rex::devices
