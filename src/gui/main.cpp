#include "mainwindow.h"
#include "../qemu/qemu_process.h"
#include "../qemu/qemu_config.h"
#include "../vnc/vnc_client.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QStyleFactory>
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

    QCommandLineOption kernel_opt({"k", "kernel"}, "Kernel image path.", "file");
    QCommandLineOption system_opt({"s", "system-image"}, "System image path.", "file");
    QCommandLineOption vendor_opt("vendor", "Vendor image path.", "file");
    QCommandLineOption userdata_opt("userdata", "Userdata image path.", "file");
    QCommandLineOption cache_opt("cache", "Cache image path.", "file");
    QCommandLineOption cpus_opt("cpus", "Number of vCPUs (default: 4).", "n", "4");
    QCommandLineOption ram_opt("ram", "RAM in MB (default: 4096).", "mb", "4096");
    QCommandLineOption initrd_opt("initrd", "Initrd/initramfs path.", "file");
    QCommandLineOption qemu_opt("qemu-binary", "Path to QEMU binary.", "file");
    QCommandLineOption bios_opt("bios", "BIOS/bootloader path (e.g. U-Boot).", "file");
    QCommandLineOption adb_port_opt("adb-port", "ADB host port (default: 5555, 0=disable).", "port", "5555");
    QCommandLineOption frida_port_opt("frida-port", "Frida host port (default: 27042, 0=disable).", "port", "27042");

    parser.addOption(kernel_opt);
    parser.addOption(system_opt);
    parser.addOption(vendor_opt);
    parser.addOption(userdata_opt);
    parser.addOption(cache_opt);
    parser.addOption(cpus_opt);
    parser.addOption(ram_opt);
    parser.addOption(initrd_opt);
    parser.addOption(qemu_opt);
    parser.addOption(bios_opt);
    parser.addOption(adb_port_opt);
    parser.addOption(frida_port_opt);
    parser.process(app);

    // Build QEMU config
    rex::qemu::QemuConfig qemu_config;
    qemu_config.vcpus = parser.value(cpus_opt).toUInt();
    qemu_config.ram_mb = parser.value(ram_opt).toUInt();
    qemu_config.adb_host_port = static_cast<uint16_t>(parser.value(adb_port_opt).toUInt());
    qemu_config.frida_host_port = static_cast<uint16_t>(parser.value(frida_port_opt).toUInt());

    if (parser.isSet(kernel_opt))
        qemu_config.kernel_path = parser.value(kernel_opt);
    if (parser.isSet(system_opt))
        qemu_config.system_image_path = parser.value(system_opt);
    if (parser.isSet(vendor_opt))
        qemu_config.vendor_image_path = parser.value(vendor_opt);
    if (parser.isSet(userdata_opt))
        qemu_config.userdata_image_path = parser.value(userdata_opt);
    if (parser.isSet(cache_opt))
        qemu_config.cache_image_path = parser.value(cache_opt);
    if (parser.isSet(initrd_opt))
        qemu_config.initrd_path = parser.value(initrd_opt);
    if (parser.isSet(qemu_opt))
        qemu_config.qemu_binary = parser.value(qemu_opt);
    if (parser.isSet(bios_opt)) {
        qemu_config.bios_path = parser.value(bios_opt);
        qemu_config.boot_mode = rex::qemu::QemuConfig::BootMode::Bios;
    }

    // Set kernel cmdline
    if (!qemu_config.kernel_path.isEmpty() && qemu_config.cmdline.isEmpty()) {
        if (!qemu_config.system_image_path.isEmpty()) {
            // Android boot — use full Android cmdline
            qemu_config.cmdline = rex::qemu::QemuConfig::androidCmdline();
        } else {
            // Plain Linux boot
            qemu_config.cmdline =
                "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000";
        }
    }

    qemu_config.generateSocketPaths("");

    auto* qemu = new rex::qemu::QemuProcess();
    auto* vnc = new rex::vnc::VncClient();

    // After QMP ready, query actual VNC port then connect
    QObject::connect(qemu, &rex::qemu::QemuProcess::qmpReady, [vnc, qemu]() {
        fprintf(stderr, "main: QMP ready, querying VNC port...\n");
        qemu->qmp()->execute("query-vnc", {}, [vnc](bool ok, const QJsonObject& resp) {
            if (!ok) {
                fprintf(stderr, "main: query-vnc failed, trying default port 5900\n");
                vnc->connectToHost("127.0.0.1", 5900);
                return;
            }
            auto ret = resp["return"].toObject();
            QString host = ret["host"].toString("127.0.0.1");
            int port = ret["service"].toString("5900").toInt();
            fprintf(stderr, "main: VNC at %s:%d\n", host.toUtf8().constData(), port);
            vnc->connectToHost(host, static_cast<quint16>(port));
        });
    });

    rex::gui::MainWindow window;
    window.setQemuProcess(qemu);
    window.setVncClient(vnc);
    window.show();

    // Auto-start VM if any image is specified
    bool has_boot = !qemu_config.kernel_path.isEmpty()
                 || !qemu_config.system_image_path.isEmpty()
                 || !qemu_config.bios_path.isEmpty();
    if (has_boot)
        qemu->start(qemu_config);

    int ret = app.exec();
    delete vnc;
    delete qemu;
    return ret;
}
