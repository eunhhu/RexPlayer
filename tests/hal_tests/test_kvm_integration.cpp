#include <gtest/gtest.h>
#include "rex/hal/types.h"
#include "rex/hal/hypervisor.h"

#ifdef __linux__

#include "kvm/kvm_hypervisor.h"
#include <sys/mman.h>
#include <cstring>

using namespace rex::hal;

// ---------------------------------------------------------------------------
// Tiny x86 real-mode guest that writes "REX" to port 0x3F8 (COM1), then HLTs.
//
// Assembly (AT&T syntax, 16-bit real mode):
//   mov $'R', %al      ; b0 52
//   out %al, $0x3f8     ; e6 -> but we need 16-bit out to dx
//   ...
//
// We use: mov dx, 0x3F8 / mov al, char / out dx, al / ... / hlt
//
// Machine code (16-bit real mode):
//   BA F8 03       mov dx, 0x03F8
//   B0 52          mov al, 'R'
//   EE             out dx, al
//   B0 45          mov al, 'E'
//   EE             out dx, al
//   B0 58          mov al, 'X'
//   EE             out dx, al
//   F4             hlt
// ---------------------------------------------------------------------------
static constexpr uint8_t kGuestCode[] = {
    0xBA, 0xF8, 0x03,   // mov dx, 0x03F8
    0xB0, 0x52,         // mov al, 'R'
    0xEE,               // out dx, al
    0xB0, 0x45,         // mov al, 'E'
    0xEE,               // out dx, al
    0xB0, 0x58,         // mov al, 'X'
    0xEE,               // out dx, al
    0xF4,               // hlt
};

static constexpr size_t kGuestMemorySize = 4096; // one page

class KvmIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        hv_ = std::make_unique<KvmHypervisor>();
        if (!hv_->is_available()) {
            GTEST_SKIP() << "KVM not available on this system";
        }
    }

    std::unique_ptr<KvmHypervisor> hv_;
};

TEST_F(KvmIntegrationTest, GuestWritesRexToUart) {
    // 1. Initialize hypervisor
    auto init_result = hv_->initialize();
    ASSERT_TRUE(init_result.has_value())
        << "Failed to initialize KVM: " << hal_error_str(init_result.error());

    EXPECT_EQ(hv_->api_version(), 12) << "Unexpected KVM API version";

    // 2. Create a VM (no IRQ chip needed for this simple test)
    VmConfig config{};
    config.num_vcpus = 1;
    config.ram_size = kGuestMemorySize;
    config.enable_irqchip = false;

    auto vm_result = hv_->create_vm(config);
    ASSERT_TRUE(vm_result.has_value())
        << "Failed to create VM: " << hal_error_str(vm_result.error());

    // 3. Allocate and map guest memory
    void* guest_mem = mmap(nullptr, kGuestMemorySize,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                           -1, 0);
    ASSERT_NE(guest_mem, MAP_FAILED) << "mmap failed for guest memory";

    // Copy guest code to the start of guest memory (GPA 0x0)
    std::memcpy(guest_mem, kGuestCode, sizeof(kGuestCode));

    MemoryRegion region{};
    region.slot = 0;
    region.guest_phys_addr = 0;
    region.size = kGuestMemorySize;
    region.userspace_addr = reinterpret_cast<uint64_t>(guest_mem);
    region.readonly = false;

    auto map_result = hv_->memory_manager().map_region(region);
    ASSERT_TRUE(map_result.has_value())
        << "Failed to map memory: " << hal_error_str(map_result.error());

    // 4. Create a vCPU
    auto vcpu_result = hv_->create_vcpu(0);
    ASSERT_TRUE(vcpu_result.has_value())
        << "Failed to create vCPU: " << hal_error_str(vcpu_result.error());

    auto& vcpu = *vcpu_result.value();

    // 5. Set up special registers for real mode
    auto sregs_result = vcpu.get_sregs();
    ASSERT_TRUE(sregs_result.has_value());

    auto sregs = sregs_result.value();
    // Code segment: base=0, execute at GPA 0x0
    sregs.cs.base = 0;
    sregs.cs.selector = 0;

    auto set_sregs_result = vcpu.set_sregs(sregs);
    ASSERT_TRUE(set_sregs_result.has_value());

    // 6. Set general registers — RIP = 0 (start of guest code), RFLAGS = 0x2
    X86Regs regs{};
    regs.rip = 0;
    regs.rflags = 0x2; // bit 1 always set on x86

    auto set_regs_result = vcpu.set_regs(regs);
    ASSERT_TRUE(set_regs_result.has_value());

    // 7. Run the vCPU and collect I/O exits
    std::string uart_output;
    bool halted = false;
    constexpr int kMaxExits = 100; // safety limit

    for (int i = 0; i < kMaxExits && !halted; ++i) {
        auto exit_result = vcpu.run();
        ASSERT_TRUE(exit_result.has_value())
            << "vCPU run failed: " << hal_error_str(exit_result.error());

        const auto& exit = exit_result.value();

        switch (exit.reason) {
            case VcpuExit::Reason::IoAccess:
                // We expect writes to port 0x3F8
                EXPECT_EQ(exit.io.port, 0x3F8);
                EXPECT_TRUE(exit.io.is_write);
                EXPECT_EQ(exit.io.size, 1);
                uart_output += static_cast<char>(exit.io.data & 0xFF);
                break;

            case VcpuExit::Reason::Hlt:
                halted = true;
                break;

            default:
                FAIL() << "Unexpected exit reason: "
                       << static_cast<uint32_t>(exit.reason);
        }
    }

    // 8. Verify the guest wrote "REX"
    EXPECT_TRUE(halted) << "Guest did not HLT within exit limit";
    EXPECT_EQ(uart_output, "REX")
        << "Expected guest to write \"REX\" to UART, got \"" << uart_output << "\"";

    // 9. Verify final register state
    auto final_regs = vcpu.get_regs();
    ASSERT_TRUE(final_regs.has_value());

    // RIP should be past all instructions (sizeof guest code)
    // After HLT, RIP points to the byte after HLT
    EXPECT_EQ(final_regs->rip, sizeof(kGuestCode));

    // DX should still hold 0x3F8
    EXPECT_EQ(final_regs->rdx, 0x3F8);

    // AL (low byte of RAX) should be 'X' (the last character written)
    EXPECT_EQ(final_regs->rax & 0xFF, static_cast<uint64_t>('X'));

    // Cleanup
    munmap(guest_mem, kGuestMemorySize);
}

TEST_F(KvmIntegrationTest, ApiVersionIs12) {
    auto init_result = hv_->initialize();
    ASSERT_TRUE(init_result.has_value());

    EXPECT_EQ(hv_->api_version(), 12);
    EXPECT_EQ(hv_->name(), "KVM");
}

TEST_F(KvmIntegrationTest, MemoryManagerGpaToHva) {
    auto init_result = hv_->initialize();
    ASSERT_TRUE(init_result.has_value());

    VmConfig config{};
    config.num_vcpus = 1;
    config.ram_size = kGuestMemorySize;
    config.enable_irqchip = false;

    auto vm_result = hv_->create_vm(config);
    ASSERT_TRUE(vm_result.has_value());

    void* guest_mem = mmap(nullptr, kGuestMemorySize,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                           -1, 0);
    ASSERT_NE(guest_mem, MAP_FAILED);

    MemoryRegion region{};
    region.slot = 0;
    region.guest_phys_addr = 0x1000;
    region.size = kGuestMemorySize;
    region.userspace_addr = reinterpret_cast<uint64_t>(guest_mem);
    region.readonly = false;

    auto map_result = hv_->memory_manager().map_region(region);
    ASSERT_TRUE(map_result.has_value());

    // Translate GPA within the mapped region
    auto hva = hv_->memory_manager().gpa_to_hva(0x1000);
    ASSERT_TRUE(hva.has_value());
    EXPECT_EQ(*hva, reinterpret_cast<uint64_t>(guest_mem));

    // Translate GPA in the middle of the region
    auto hva_mid = hv_->memory_manager().gpa_to_hva(0x1000 + 256);
    ASSERT_TRUE(hva_mid.has_value());
    EXPECT_EQ(*hva_mid, reinterpret_cast<uint64_t>(guest_mem) + 256);

    // GPA outside the region should fail
    auto hva_bad = hv_->memory_manager().gpa_to_hva(0x0);
    EXPECT_FALSE(hva_bad.has_value());

    munmap(guest_mem, kGuestMemorySize);
}

#else // !__linux__

// On non-Linux platforms, the tests compile but are skipped at runtime
TEST(KvmIntegration, SkippedOnNonLinux) {
    GTEST_SKIP() << "KVM tests only run on Linux";
}

#endif // __linux__
