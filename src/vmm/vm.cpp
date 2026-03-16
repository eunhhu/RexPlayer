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
    if (!init_result) return init_result;

    // Create the VM
    rex::hal::VmConfig vm_config{};
    vm_config.num_vcpus = config.num_vcpus;
    vm_config.ram_size = config.ram_size;
    vm_config.enable_irqchip = true;

    auto vm_result = hypervisor_->create_vm(vm_config);
    if (!vm_result) return vm_result;

    // Set up memory
    mem_mgr_ = std::make_unique<MemoryManager>(hypervisor_->memory_manager());

    // Map guest RAM at GPA 0
    auto ram_result = mem_mgr_->add_ram(0, config.ram_size);
    if (!ram_result) return ram_result;

    // Create vCPUs
    for (uint32_t i = 0; i < config.num_vcpus; ++i) {
        auto vcpu_result = hypervisor_->create_vcpu(i);
        if (!vcpu_result) return std::unexpected(vcpu_result.error());
        vcpus_.push_back(std::move(*vcpu_result));
    }

    // Set up direct kernel boot on the BSP (vCPU 0)
    if (!config.boot.kernel_path.empty()) {
#if defined(__aarch64__)
        auto boot_result = setup_direct_boot_arm64(*vcpus_[0], *mem_mgr_, config.boot);
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
                if (!device_mgr_.dispatch_mmio(exit.mmio)) {
                    // Unhandled MMIO
                    if (!exit.mmio.is_write) {
                        exit.mmio.data = 0;
                    }
                }
                break;
            }
            case rex::hal::VcpuExit::Reason::Hlt:
                // Guest executed HLT — wait for interrupt
                // In a real implementation, we'd wait on an eventfd/signal
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
