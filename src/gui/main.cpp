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
    QCommandLineOption cpus_opt("cpus", "Number of vCPUs (default: 4).", "n", "4");
    QCommandLineOption ram_opt("ram", "RAM in MB (default: 4096).", "mb", "4096");
    QCommandLineOption initrd_opt("initrd", "Initrd/initramfs path.", "file");
    QCommandLineOption qemu_opt("qemu-binary", "Path to QEMU binary.", "file");
    parser.addOption(kernel_opt);
    parser.addOption(system_opt);
    parser.addOption(cpus_opt);
    parser.addOption(ram_opt);
    parser.addOption(initrd_opt);
    parser.addOption(qemu_opt);
    parser.process(app);

    rex::qemu::QemuConfig qemu_config;
    qemu_config.vcpus = parser.value(cpus_opt).toUInt();
    qemu_config.ram_mb = parser.value(ram_opt).toUInt();
    if (parser.isSet(kernel_opt))
        qemu_config.kernel_path = parser.value(kernel_opt);
    if (parser.isSet(system_opt))
        qemu_config.system_image_path = parser.value(system_opt);
    if (parser.isSet(initrd_opt))
        qemu_config.initrd_path = parser.value(initrd_opt);
    if (parser.isSet(qemu_opt))
        qemu_config.qemu_binary = parser.value(qemu_opt);
    if (!qemu_config.kernel_path.isEmpty()) {
        qemu_config.cmdline =
            "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000 "
            "androidboot.hardware=rex androidboot.selinux=permissive";
    }
    qemu_config.generateSocketPaths("");

    auto* qemu = new rex::qemu::QemuProcess();
    auto* vnc = new rex::vnc::VncClient();

    // Connect VNC after QEMU QMP is ready
    QObject::connect(qemu, &rex::qemu::QemuProcess::qmpReady, [vnc, &qemu_config]() {
        fprintf(stderr, "main: QMP ready, connecting VNC on port %d...\n", qemu_config.vnc_port);
        vnc->connectToHost("127.0.0.1", qemu_config.vnc_port);
    });

    rex::gui::MainWindow window;
    window.setQemuProcess(qemu);
    window.setVncClient(vnc);
    window.show();

    if (!qemu_config.kernel_path.isEmpty() || !qemu_config.system_image_path.isEmpty())
        qemu->start(qemu_config);

    int ret = app.exec();
    delete vnc;
    delete qemu;
    return ret;
}
