#pragma once

#include "rex/vmm/device_manager.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>

namespace rex::devices {

/// UART 16550 serial port emulation
///
/// Provides COM1 (0x3F8) serial console for kernel boot messages.
/// Supports basic FIFO transmit/receive for guest console I/O.
class Uart16550 : public rex::vmm::IDevice {
public:
    /// Callback for characters transmitted by the guest
    using OutputCallback = std::function<void(char)>;

    explicit Uart16550(OutputCallback on_output = nullptr);

    std::string name() const override { return "UART 16550"; }
    void io_access(rex::hal::IoAccess& access) override;
    void mmio_access(rex::hal::MmioAccess& access) override;

    /// Inject a character into the receive buffer (host → guest)
    void inject_input(char c);

    /// Set the output callback
    void set_output_callback(OutputCallback cb) { on_output_ = std::move(cb); }

    // Standard COM1 I/O port base
    static constexpr uint16_t COM1_BASE = 0x3F8;
    static constexpr uint16_t PORT_COUNT = 8;

private:
    // Register offsets from base
    enum Reg : uint16_t {
        RBR_THR = 0, // Receive Buffer / Transmit Holding (DLAB=0)
        IER     = 1, // Interrupt Enable
        IIR_FCR = 2, // Interrupt Ident (read) / FIFO Control (write)
        LCR     = 3, // Line Control
        MCR     = 4, // Modem Control
        LSR     = 5, // Line Status
        MSR     = 6, // Modem Status
        SCR     = 7, // Scratch
    };

    // Line Status Register bits
    static constexpr uint8_t LSR_DR   = 0x01; // Data Ready
    static constexpr uint8_t LSR_THRE = 0x20; // Transmit Holding Register Empty
    static constexpr uint8_t LSR_TEMT = 0x40; // Transmitter Empty

    void handle_read(uint16_t offset, rex::hal::IoAccess& access);
    void handle_write(uint16_t offset, rex::hal::IoAccess& access);

    OutputCallback on_output_;
    std::mutex mutex_;

    uint8_t ier_ = 0;
    uint8_t lcr_ = 0;
    uint8_t mcr_ = 0;
    uint8_t scr_ = 0;
    uint8_t fcr_ = 0;
    bool dlab_ = false; // Divisor Latch Access Bit

    std::queue<uint8_t> rx_fifo_;
};

} // namespace rex::devices
