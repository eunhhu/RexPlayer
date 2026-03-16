#include "mainwindow.h"
#include "settings_dialog.h"
#include "../gpu/display.h"
#include "../vmm/include/rex/vmm/vm.h"
#include "../vmm/embedded_kernel.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QStyleFactory>

#include <cstdio>
#include <memory>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("RexPlayer"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("RexPlayer"));

    // Use Fusion style for consistent cross-platform appearance
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        QApplication::setStyle(QStringLiteral("Fusion"));
    }

    // --- Command-line argument parsing ---
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("RexPlayer - Lightweight Android app player"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption config_opt(
        QStringList() << QStringLiteral("c") << QStringLiteral("config"),
        QStringLiteral("Path to TOML configuration file."),
        QStringLiteral("file"));
    parser.addOption(config_opt);

    QCommandLineOption kernel_opt(
        QStringList() << QStringLiteral("k") << QStringLiteral("kernel"),
        QStringLiteral("Path to the kernel image (bzImage / Image)."),
        QStringLiteral("file"));
    parser.addOption(kernel_opt);

    QCommandLineOption system_opt(
        QStringList() << QStringLiteral("s") << QStringLiteral("system-image"),
        QStringLiteral("Path to the Android system image."),
        QStringLiteral("file"));
    parser.addOption(system_opt);

    QCommandLineOption cpus_opt(
        QStringLiteral("cpus"),
        QStringLiteral("Number of vCPU cores (default: 2)."),
        QStringLiteral("n"), QStringLiteral("2"));
    parser.addOption(cpus_opt);

    QCommandLineOption ram_opt(
        QStringLiteral("ram"),
        QStringLiteral("RAM size in MB (default: 2048)."),
        QStringLiteral("mb"), QStringLiteral("2048"));
    parser.addOption(ram_opt);

    QCommandLineOption width_opt(
        QStringLiteral("width"),
        QStringLiteral("Display width in pixels (default: 1080)."),
        QStringLiteral("px"), QStringLiteral("1080"));
    parser.addOption(width_opt);

    QCommandLineOption height_opt(
        QStringLiteral("height"),
        QStringLiteral("Display height in pixels (default: 1920)."),
        QStringLiteral("px"), QStringLiteral("1920"));
    parser.addOption(height_opt);

    parser.process(app);

    if (parser.isSet(config_opt)) {
        fprintf(stderr,
                "--config is not supported by the current native runtime; "
                "use explicit CLI overrides instead.\n");
        return 2;
    }

    if (parser.isSet(system_opt)) {
        fprintf(stderr,
                "--system-image is not wired into the current native runtime; "
                "direct kernel boot is the only supported launch path.\n");
        return 2;
    }

    // --- Build configuration from CLI args ---
    rex::gui::RexConfig config;

    if (parser.isSet(cpus_opt)) {
        config.cpu_cores = static_cast<uint32_t>(parser.value(cpus_opt).toUInt());
    }
    if (parser.isSet(ram_opt)) {
        config.ram_mb = static_cast<uint32_t>(parser.value(ram_opt).toUInt());
    }
    if (parser.isSet(width_opt)) {
        config.display_width = static_cast<uint32_t>(parser.value(width_opt).toUInt());
    }
    if (parser.isSet(height_opt)) {
        config.display_height = static_cast<uint32_t>(parser.value(height_opt).toUInt());
    }

    // --- Create the GPU display backend ---
    auto gpu_display = std::make_unique<rex::gpu::Display>();
    gpu_display->resize(config.display_width, config.display_height);

    // --- Create the VM ---
    // If no kernel is specified, generate a built-in test kernel automatically.
    std::unique_ptr<rex::vmm::Vm> vm;

    {
        std::string kernel_path;

        if (parser.isSet(kernel_opt)) {
            kernel_path = parser.value(kernel_opt).toStdString();
        } else {
            // Generate embedded test kernel for the current architecture
#if defined(__aarch64__)
            auto kernel = rex::vmm::generate_test_kernel_arm64();
            kernel_path = rex::vmm::save_temp_kernel(kernel, "rex_test_arm64.bin");
            fprintf(stderr, "No kernel specified — using built-in ARM64 test kernel\n");
#elif defined(__x86_64__)
            auto kernel = rex::vmm::generate_test_kernel_x86();
            kernel_path = rex::vmm::save_temp_kernel(kernel, "rex_test_x86.bin");
            fprintf(stderr, "No kernel specified — using built-in x86 test kernel\n");
#endif
        }

        if (!kernel_path.empty()) {
            vm = std::make_unique<rex::vmm::Vm>();

            rex::vmm::VmCreateConfig vm_config;
            vm_config.num_vcpus = config.cpu_cores;
            vm_config.ram_size = static_cast<uint64_t>(config.ram_mb) * 1024 * 1024;
            vm_config.boot.kernel_path = kernel_path;
            vm_config.boot.cmdline =
                "console=ttyS0 androidboot.hardware=rex "
                "androidboot.selinux=permissive";

            auto result = vm->create(vm_config);
            if (!result) {
                fprintf(stderr, "Failed to create VM: %s\n",
                        rex::hal::hal_error_str(result.error()));
                vm.reset();
            } else {
                fprintf(stderr, "VM created: %u vCPUs, %u MB RAM\n",
                        config.cpu_cores, config.ram_mb);
            }
        }
    }

    // --- Create and show the main window ---
    rex::gui::MainWindow window;
    window.applyConfig(config);
    window.setDisplay(gpu_display.get());

    if (vm) {
        window.setVm(vm.get());
        // Auto-start the VM
        auto start_result = vm->start();
        if (start_result) {
            fprintf(stderr, "VM started — vCPU running\n");
        } else {
            fprintf(stderr, "VM start failed: %s\n",
                    rex::hal::hal_error_str(start_result.error()));
        }
    }

    window.show();

    return app.exec();
}
