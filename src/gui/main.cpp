#include "mainwindow.h"
#include "../qemu/qemu_process.h"
#include "../qemu/qemu_config.h"
#include "../vnc/vnc_client.h"
#include "../emu/emulator_process.h"
#include "../emu/grpc_display.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QStyleFactory>
#include <QMessageBox>
#include <cstdio>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("RexPlayer");
    QApplication::setApplicationVersion("0.2.0");
    QApplication::setOrganizationName("RexPlayer");

    if (QStyleFactory::keys().contains("Fusion"))
        QApplication::setStyle("Fusion");

    QCommandLineParser parser;
    parser.setApplicationDescription("RexPlayer - Android app player powered by QEMU");
    parser.addHelpOption();
    parser.addVersionOption();

    // Emulator mode options
    QCommandLineOption avd_opt("avd", "AVD name (auto-detect if omitted).", "name");
    QCommandLineOption grpc_port_opt("grpc-port", "gRPC port (default: 8554).", "port", "8554");
    QCommandLineOption gpu_opt("gpu", "GPU mode (default: swiftshader_indirect).", "mode", "swiftshader_indirect");

    // QEMU direct mode options
    QCommandLineOption kernel_opt({"k", "kernel"}, "Kernel image (enables QEMU direct mode).", "file");
    QCommandLineOption system_opt({"s", "system-image"}, "System image path.", "file");
    QCommandLineOption vendor_opt("vendor", "Vendor image path.", "file");
    QCommandLineOption userdata_opt("userdata", "Userdata image path.", "file");
    QCommandLineOption cache_opt("cache", "Cache image path.", "file");
    QCommandLineOption initrd_opt("initrd", "Initrd path.", "file");
    QCommandLineOption qemu_opt("qemu-binary", "QEMU binary path.", "file");

    // Common options
    QCommandLineOption cpus_opt("cpus", "vCPU count (default: 4).", "n", "4");
    QCommandLineOption ram_opt("ram", "RAM in MB (default: 4096).", "mb", "4096");

    parser.addOption(avd_opt);
    parser.addOption(grpc_port_opt);
    parser.addOption(gpu_opt);
    parser.addOption(kernel_opt);
    parser.addOption(system_opt);
    parser.addOption(vendor_opt);
    parser.addOption(userdata_opt);
    parser.addOption(cache_opt);
    parser.addOption(initrd_opt);
    parser.addOption(qemu_opt);
    parser.addOption(cpus_opt);
    parser.addOption(ram_opt);
    parser.process(app);

    bool qemu_mode = parser.isSet(kernel_opt);

    rex::gui::MainWindow window;
    window.show();

    if (qemu_mode) {
        // --- QEMU direct mode (Linux kernel boot) ---
        fprintf(stderr, "main: QEMU direct mode\n");

        auto* qemu = new rex::qemu::QemuProcess();
        auto* vnc = new rex::vnc::VncClient();

        rex::qemu::QemuConfig config;
        config.vcpus = parser.value(cpus_opt).toUInt();
        config.ram_mb = parser.value(ram_opt).toUInt();
        config.kernel_path = parser.value(kernel_opt);
        if (parser.isSet(system_opt)) config.system_image_path = parser.value(system_opt);
        if (parser.isSet(vendor_opt)) config.vendor_image_path = parser.value(vendor_opt);
        if (parser.isSet(userdata_opt)) config.userdata_image_path = parser.value(userdata_opt);
        if (parser.isSet(cache_opt)) config.cache_image_path = parser.value(cache_opt);
        if (parser.isSet(initrd_opt)) config.initrd_path = parser.value(initrd_opt);
        if (parser.isSet(qemu_opt)) config.qemu_binary = parser.value(qemu_opt);
        if (config.cmdline.isEmpty()) {
            config.cmdline = config.system_image_path.isEmpty()
                ? "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000"
                : rex::qemu::QemuConfig::androidCmdline();
        }
        config.generateSocketPaths("");

        QObject::connect(qemu, &rex::qemu::QemuProcess::qmpReady, [vnc, qemu]() {
            qemu->qmp()->execute("query-vnc", {}, [vnc](bool ok, const QJsonObject& resp) {
                auto ret = resp["return"].toObject();
                int port = ok ? ret["service"].toString("5900").toInt() : 5900;
                fprintf(stderr, "main: VNC at port %d\n", port);
                vnc->connectToHost("127.0.0.1", static_cast<quint16>(port));
            });
        });

        window.setQemuProcess(qemu);
        window.setVncClient(vnc);
        qemu->start(config);

    } else {
        // --- Emulator mode (default — Android with gRPC) ---
        fprintf(stderr, "main: Emulator mode (gRPC)\n");

        auto* emu = new rex::emu::EmulatorProcess();
        auto* grpc = new rex::emu::GrpcDisplay();

        rex::emu::EmulatorProcess::Config config;
        config.grpc_port = static_cast<uint16_t>(parser.value(grpc_port_opt).toUInt());
        config.gpu_mode = parser.value(gpu_opt);

        // Find AVD
        if (parser.isSet(avd_opt)) {
            config.avd_name = parser.value(avd_opt);
        } else {
            config.avd_name = rex::emu::EmulatorProcess::findOrCreateAvd();
            if (config.avd_name.isEmpty()) {
                QMessageBox::critical(nullptr, "RexPlayer",
                    "No Android Virtual Device (AVD) found.\n\n"
                    "Please create one in Android Studio or run:\n"
                    "  avdmanager create avd -n RexPlayer "
                    "-k 'system-images;android-36.1;google_apis_playstore;arm64-v8a' "
                    "-d pixel");
                return 1;
            }
        }

        fprintf(stderr, "main: using AVD '%s', gRPC port %d\n",
                config.avd_name.toUtf8().constData(), config.grpc_port);

        // Connect gRPC after emulator boots
        QObject::connect(emu, &rex::emu::EmulatorProcess::started, [grpc, &config]() {
            fprintf(stderr, "main: emulator booted, connecting gRPC...\n");
            grpc->connectToEmulator("127.0.0.1", config.grpc_port);
        });

        window.setEmulatorProcess(emu);
        window.setGrpcDisplay(grpc);
        emu->start(config);
    }

    return app.exec();
}
