#include <gtest/gtest.h>
#include "rex/vmm/device_manager.h"
#include "legacy/uart_16550.h"
#include "legacy/i8042.h"
#include "legacy/rtc.h"
#include "legacy/pci_host.h"

using namespace rex::hal;
using namespace rex::vmm;
using namespace rex::devices;

// --- DeviceManager Tests ---

TEST(DeviceManager, DispatchIoToRegisteredDevice) {
    DeviceManager mgr;

    char last_char = 0;
    auto uart = std::make_shared<Uart16550>([&last_char](char c) {
        last_char = c;
    });

    mgr.register_io(Uart16550::COM1_BASE, Uart16550::PORT_COUNT, uart);

    // Write 'X' to THR (transmit holding register)
    IoAccess write_access{};
    write_access.port = 0x3F8;
    write_access.size = 1;
    write_access.is_write = true;
    write_access.data = 'X';

    bool handled = mgr.dispatch_io(write_access);
    EXPECT_TRUE(handled);
    EXPECT_EQ(last_char, 'X');
}

TEST(DeviceManager, UnhandledIoReturns_false) {
    DeviceManager mgr;

    IoAccess access{};
    access.port = 0x1234;
    access.is_write = false;

    bool handled = mgr.dispatch_io(access);
    EXPECT_FALSE(handled);
}

TEST(DeviceManager, DispatchMmioUnhandled) {
    DeviceManager mgr;

    MmioAccess access{};
    access.address = 0xDEAD0000;
    access.size = 4;
    access.is_write = false;

    bool handled = mgr.dispatch_mmio(access);
    EXPECT_FALSE(handled);
}

// --- UART Tests ---

TEST(Uart16550, TransmitCharacter) {
    std::string output;
    Uart16550 uart([&output](char c) {
        output += c;
    });

    IoAccess access{};
    access.port = 0x3F8;
    access.size = 1;
    access.is_write = true;

    access.data = 'H';
    uart.io_access(access);
    access.data = 'i';
    uart.io_access(access);

    EXPECT_EQ(output, "Hi");
}

TEST(Uart16550, LineStatusAlwaysReady) {
    Uart16550 uart;

    IoAccess access{};
    access.port = 0x3F8 + 5; // LSR
    access.size = 1;
    access.is_write = false;
    access.data = 0;

    uart.io_access(access);

    // THRE and TEMT should be set (transmitter ready)
    EXPECT_TRUE(access.data & 0x20);
    EXPECT_TRUE(access.data & 0x40);
}

TEST(Uart16550, ReceiveCharacter) {
    Uart16550 uart;

    uart.inject_input('A');

    // Check LSR — data ready bit should be set
    IoAccess lsr{};
    lsr.port = 0x3F8 + 5;
    lsr.size = 1;
    lsr.is_write = false;
    uart.io_access(lsr);
    EXPECT_TRUE(lsr.data & 0x01);

    // Read the character
    IoAccess read{};
    read.port = 0x3F8;
    read.size = 1;
    read.is_write = false;
    uart.io_access(read);
    EXPECT_EQ(read.data, static_cast<uint32_t>('A'));
}

// --- RTC Tests ---

TEST(Rtc, ReturnsValidTime) {
    Rtc rtc;

    // Write index register to select seconds
    IoAccess idx{};
    idx.port = 0x70;
    idx.size = 1;
    idx.is_write = true;
    idx.data = 0x00; // seconds register
    rtc.io_access(idx);

    // Read data register
    IoAccess data{};
    data.port = 0x71;
    data.size = 1;
    data.is_write = false;
    rtc.io_access(data);

    // BCD seconds should be 0-59 → BCD 0x00-0x59
    EXPECT_LE(data.data, 0x59u);
}

TEST(Rtc, StatusRegisterD) {
    Rtc rtc;

    IoAccess idx{};
    idx.port = 0x70;
    idx.size = 1;
    idx.is_write = true;
    idx.data = 0x0D;
    rtc.io_access(idx);

    IoAccess data{};
    data.port = 0x71;
    data.size = 1;
    data.is_write = false;
    rtc.io_access(data);

    // Bit 7 should be set (valid RAM/time)
    EXPECT_TRUE(data.data & 0x80);
}

// --- i8042 Tests ---

TEST(I8042, StatusPortReturnsZero) {
    I8042 kbd;

    IoAccess access{};
    access.port = 0x64;
    access.size = 1;
    access.is_write = false;
    kbd.io_access(access);

    EXPECT_EQ(access.data, 0x00u);
}

// --- PCI Host Tests ---

TEST(PciHost, NoDeviceReturnsAllOnes) {
    PciHost pci;

    // Write config address: enable bit + bus 0, device 0, function 0, offset 0
    IoAccess addr{};
    addr.port = 0x0CF8;
    addr.size = 4;
    addr.is_write = true;
    addr.data = 0x80000000; // enable + BDF 0:0.0
    pci.io_access(addr);

    // Read config data — should return 0xFFFFFFFF (no device)
    IoAccess data{};
    data.port = 0x0CFC;
    data.size = 4;
    data.is_write = false;
    pci.io_access(data);

    EXPECT_EQ(data.data, 0xFFFFFFFFu);
}
