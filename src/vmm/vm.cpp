#include "rex/vmm/vm.h"
#include <cstdio>
#include <exception>
#include <fstream>

namespace {

constexpr size_t kArm64MagicOffset = 56;
constexpr uint32_t kArm64ImageMagic = 0x644D5241; // "ARM\x64"

bool looks_like_arm64_kernel(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < static_cast<std::streamoff>(kArm64MagicOffset + sizeof(uint32_t))) {
        return false;
    }

    file.seekg(kArm64MagicOffset, std::ios::beg);
    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return file.good() && magic == kArm64ImageMagic;
}

} // namespace

namespace rex::vmm {

Vm::Vm() = default;

Vm::~Vm() {
    stop();
}

rex::hal::HalResult<void> Vm::create(const VmCreateConfig& config) {
    config_ = config;

    // Create the hypervisor backend for the current platform
    try {
        hypervisor_ = rex::hal::create_hypervisor();
    } catch (const std::exception&) {
        return std::unexpected(rex::hal::HalError::NotSupported);
    }

    auto init_result = hypervisor_->initialize();
    if (!init_result) { fprintf(stderr, "vm: init failed: %s\n", rex::hal::hal_error_str(init_result.error())); return init_result; }
    fprintf(stderr, "vm: hypervisor %s initialized\n", hypervisor_->name().c_str());

    // Create the VM
    rex::hal::VmConfig vm_config{};
    vm_config.num_vcpus = config.num_vcpus;
    vm_config.ram_size = config.ram_size;
    vm_config.enable_irqchip = true;

    auto vm_result = hypervisor_->create_vm(vm_config);
    if (!vm_result) { fprintf(stderr, "vm: create_vm failed: %s\n", rex::hal::hal_error_str(vm_result.error())); return vm_result; }
    fprintf(stderr, "vm: VM created\n");

    // Set up memory
    mem_mgr_ = std::make_unique<MemoryManager>(hypervisor_->memory_manager());

    // Map guest RAM at GPA 0
    auto ram_result = mem_mgr_->add_ram(0, config.ram_size);
    if (!ram_result) { fprintf(stderr, "vm: add_ram failed: %s\n", rex::hal::hal_error_str(ram_result.error())); return ram_result; }
    fprintf(stderr, "vm: %llu MB RAM mapped at GPA 0\n", config.ram_size / (1024*1024));

    // Create vCPUs
    for (uint32_t i = 0; i < config.num_vcpus; ++i) {
        auto vcpu_result = hypervisor_->create_vcpu(i);
        if (!vcpu_result) { fprintf(stderr, "vm: create_vcpu %u failed: %s\n", i, rex::hal::hal_error_str(vcpu_result.error())); return std::unexpected(vcpu_result.error()); }
        vcpus_.push_back(std::move(*vcpu_result));
    }
    fprintf(stderr, "vm: %u vCPUs created\n", config.num_vcpus);

    // Set up direct kernel boot on the BSP (vCPU 0)
    if (!config.boot.kernel_path.empty()) {
#if defined(__aarch64__)
        auto boot_result = setup_direct_boot_arm64(*vcpus_[0], *mem_mgr_, config.boot);
        if (!boot_result) { fprintf(stderr, "vm: arm64 boot setup failed: %s\n", rex::hal::hal_error_str(boot_result.error())); }
#else
        if (looks_like_arm64_kernel(config.boot.kernel_path)) {
            return std::unexpected(rex::hal::HalError::NotSupported);
        }
        auto boot_result = setup_direct_boot_x86(*vcpus_[0], *mem_mgr_, config.boot);
#endif
        if (!boot_result) return boot_result;
    }

    state_.store(VmState::Created);
    return {};
}

rex::hal::HalResult<void> Vm::start() {
    if (state_.load() != VmState::Created && state_.load() != VmState::Paused) {
        return std::unexpected(rex::hal::HalError::InvalidParameter);
    }

    should_stop_.store(false);
    state_.store(VmState::Running);

    // Launch a thread per vCPU
    for (auto& vcpu : vcpus_) {
        vcpu_threads_.emplace_back(&Vm::vcpu_loop, this, vcpu.get());
    }

    return {};
}

void Vm::pause() {
    state_.store(VmState::Paused);
    // vCPU threads will check state_ and yield
}

void Vm::resume() {
    if (state_.load() == VmState::Paused) {
        state_.store(VmState::Running);
    }
}

void Vm::stop() {
    should_stop_.store(true);
    state_.store(VmState::Stopped);

    for (auto& t : vcpu_threads_) {
        if (t.joinable()) t.join();
    }
    vcpu_threads_.clear();
}

void Vm::vcpu_loop(rex::hal::IVcpu* vcpu) {
    while (!should_stop_.load(std::memory_order_relaxed)) {
        if (state_.load(std::memory_order_relaxed) == VmState::Paused) {
            std::this_thread::yield();
            continue;
        }

        auto exit_result = vcpu->run();
        if (!exit_result) {
            fprintf(stderr, "vCPU %u run failed: %s\n",
                    vcpu->id(), rex::hal::hal_error_str(exit_result.error()));
            break;
        }

        auto& exit = *exit_result;
        switch (exit.reason) {
            case rex::hal::VcpuExit::Reason::IoAccess: {
                if (!device_mgr_.dispatch_io(exit.io)) {
                    // Unhandled I/O — ignore (return 0xFF for reads)
                    if (!exit.io.is_write) {
                        exit.io.data = 0xFFFFFFFF;
                    }
                }
                break;
            }
            case rex::hal::VcpuExit::Reason::MmioAccess: {
                static uint64_t mmio_count = 0;
                if (++mmio_count <= 20) {
                    fprintf(stderr, "MMIO %s addr=0x%llx size=%u data=0x%llx\n",
                            exit.mmio.is_write ? "W" : "R",
                            exit.mmio.address, exit.mmio.size,
                            exit.mmio.data);
                }
                // PL011 UART at 0x09000000 — ARM64 Linux console
                constexpr uint64_t PL011_BASE = 0x09000000;
                constexpr uint64_t PL011_END  = 0x09001000;

                if (exit.mmio.address >= PL011_BASE && exit.mmio.address < PL011_END) {
                    uint32_t offset = static_cast<uint32_t>(exit.mmio.address - PL011_BASE);
                    if (exit.mmio.is_write && offset == 0x00) {
                        // UARTDR — data register: print character
                        char c = static_cast<char>(exit.mmio.data & 0xFF);
                        fputc(c, stderr);
                    } else if (!exit.mmio.is_write) {
                        // PL011 register reads
                        switch (offset) {
                            case 0x18: // UARTFR — flags: TX empty, RX empty
                                exit.mmio.data = (1 << 4) | (1 << 7); // TXFE | RXFE (ready)
                                break;
                            default:
                                exit.mmio.data = 0;
                                break;
                        }
                    }
                }
                // GIC distributor at 0x08000000 and redistributor at 0x080A0000
                else if (exit.mmio.address >= 0x08000000 && exit.mmio.address < 0x081A0000) {
                    if (!exit.mmio.is_write) {
                        uint32_t offset = static_cast<uint32_t>(exit.mmio.address - 0x08000000);
                        if (offset == 0x0004) exit.mmio.data = 0x04; // GICD_TYPER: 4 IRQ lines
                        else if (offset == 0x0000) exit.mmio.data = 0x03; // GICD_CTLR: enabled
                        else exit.mmio.data = 0;
                    }
                } else if (!device_mgr_.dispatch_mmio(exit.mmio)) {
                    if (!exit.mmio.is_write) {
                        exit.mmio.data = 0;
                    }
                }
                break;
            }
            case rex::hal::VcpuExit::Reason::Hlt:
                // Guest executed WFI/HLT — wait for interrupt
                std::this_thread::yield();
                break;
            case rex::hal::VcpuExit::Reason::Shutdown:
                fprintf(stderr, "vCPU %u: guest shutdown\n", vcpu->id());
                should_stop_.store(true);
                return;
            case rex::hal::VcpuExit::Reason::InternalError:
                fprintf(stderr, "vCPU %u: internal error\n", vcpu->id());
                should_stop_.store(true);
                return;
            default:
                break;
        }
    }
}

} // namespace rex::vmm
