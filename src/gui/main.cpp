#include "mainwindow.h"
#include "settings_dialog.h"
#include "../gpu/display.h"
#include "../vmm/include/rex/vmm/vm.h"

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

    // TODO(phase3): If --config is provided, parse the TOML file and merge
    // with CLI overrides. For now, CLI args take precedence.
    if (parser.isSet(config_opt)) {
        QString config_path = parser.value(config_opt);
        fprintf(stderr, "Config file: %s (TOML parsing not yet implemented)\n",
                config_path.toStdString().c_str());
    }

    // --- Create the GPU display backend ---
    auto gpu_display = std::make_unique<rex::gpu::Display>();
    gpu_display->resize(config.display_width, config.display_height);

    // --- Create the VM (if kernel path provided) ---
    std::unique_ptr<rex::vmm::Vm> vm;

    if (parser.isSet(kernel_opt)) {
        vm = std::make_unique<rex::vmm::Vm>();

        rex::vmm::VmCreateConfig vm_config;
        vm_config.num_vcpus = config.cpu_cores;
        vm_config.ram_size = static_cast<uint64_t>(config.ram_mb) * 1024 * 1024;
        vm_config.boot.kernel_path = parser.value(kernel_opt).toStdString();
        vm_config.boot.cmdline =
            "console=ttyS0 androidboot.hardware=rex "
            "androidboot.selinux=permissive";

        if (parser.isSet(system_opt)) {
            // System image path would be used for block device setup
            // (handled by the device layer in a later phase)
            fprintf(stderr, "System image: %s\n",
                    parser.value(system_opt).toStdString().c_str());
        }

        auto result = vm->create(vm_config);
        if (!result) {
            fprintf(stderr, "Failed to create VM: %s\n",
                    rex::hal::hal_error_str(result.error()));
            // Continue anyway — the GUI can still be used for configuration
            vm.reset();
        }
    }

    // --- Create and show the main window ---
    rex::gui::MainWindow window;
    window.applyConfig(config);
    window.setDisplay(gpu_display.get());

    if (vm) {
        window.setVm(vm.get());
    }

    window.show();

    return app.exec();
}
