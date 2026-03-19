#include "qemu_config.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

namespace rex::qemu {

QStringList QemuConfig::toCommandLine() const {
    QStringList args;

    QString binary = qemu_binary.isEmpty() ? findQemuBinary() : qemu_binary;
    args << binary;

    args << "-machine" << machine;
    args << "-cpu" << cpu;
    args << "-smp" << QString::number(vcpus);
    args << "-m" << QString::number(ram_mb);

    Accelerator accel = (accelerator == Accelerator::Auto)
                            ? detectAccelerator() : accelerator;
    switch (accel) {
        case Accelerator::HVF:  args << "-accel" << "hvf"; break;
        case Accelerator::KVM:  args << "-accel" << "kvm"; break;
        case Accelerator::WHPX: args << "-accel" << "whpx"; break;
        case Accelerator::TCG:  args << "-accel" << "tcg"; break;
        default: break;
    }

    // Boot mode
    if (boot_mode == BootMode::Bios && !bios_path.isEmpty()) {
        args << "-bios" << bios_path;
    } else {
        if (!kernel_path.isEmpty())
            args << "-kernel" << kernel_path;
        if (!initrd_path.isEmpty())
            args << "-initrd" << initrd_path;
        if (!cmdline.isEmpty())
            args << "-append" << cmdline;
    }

    // Disk images — Android uses separate partitions as virtio-blk devices
    // Order matters: vda=system, vdb=vendor, vdc=userdata, vdd=cache
    int drive_index = 0;
    auto addDrive = [&args, &drive_index](const QString& path, const QString& id) {
        if (path.isEmpty()) return;
        QString fmt = path.endsWith(".qcow2") ? "qcow2" : "raw";
        args << "-drive" << QString("if=none,id=%1,file=%2,format=%3").arg(id, path, fmt);
        args << "-device" << QString("virtio-blk-pci,drive=%1").arg(id);
        drive_index++;
    };

    if (!vendor_image_path.isEmpty()) {
        // Multi-disk Android mode
        addDrive(system_image_path, "system");
        addDrive(vendor_image_path, "vendor");
        addDrive(userdata_image_path, "userdata");
        addDrive(cache_image_path, "cache");
    } else if (!system_image_path.isEmpty()) {
        // Single disk mode (legacy or simple boot)
        args << "-drive"
             << QString("file=%1,format=raw,if=virtio").arg(system_image_path);
    }

    args << "-display" << "none";

    // Display backend
    DisplayBackend db = display_backend;
    if (db == DisplayBackend::Auto) {
        db = DisplayBackend::VNC;
    }

    if (db == DisplayBackend::SPICE && !spice_socket_path.isEmpty()) {
#ifdef Q_OS_WIN
        args << "-spice"
             << QString("port=%1,disable-ticketing=on").arg(5930);
#else
        args << "-spice"
             << QString("unix=on,addr=%1,disable-ticketing=on")
                    .arg(spice_socket_path);
#endif
    } else {
        args << "-vnc" << QString("127.0.0.1:%1,to=99").arg(vnc_port - 5900);
    }

    if (!qmp_socket_path.isEmpty()) {
#ifdef Q_OS_WIN
        args << "-qmp"
             << QString("tcp:127.0.0.1:4444,server=on,wait=off");
#else
        args << "-qmp"
             << QString("unix:%1,server=on,wait=off").arg(qmp_socket_path);
#endif
    }

    // Devices
    args << "-device" << "virtio-gpu-pci";
    args << "-device" << "virtio-keyboard-pci";
    args << "-device" << "virtio-tablet-pci";
    args << "-audiodev" << "none,id=snd0";
    args << "-device" << "intel-hda" << "-device" << "hda-duplex,audiodev=snd0";

    // Network with port forwarding
    QString netdev = "user,id=net0";
    if (adb_host_port > 0) {
        netdev += QString(",hostfwd=tcp::%1-:%2")
                      .arg(adb_host_port).arg(adb_guest_port);
    }
    if (frida_host_port > 0) {
        netdev += QString(",hostfwd=tcp::%1-:%2")
                      .arg(frida_host_port).arg(frida_guest_port);
    }
    args << "-netdev" << netdev;
    args << "-device" << "virtio-net-pci,netdev=net0";

    args << "-serial" << "mon:stdio";

    args << extra_args;

    return args;
}

QString QemuConfig::androidCmdline() {
    return "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000 "
           "androidboot.hardware=rex "
           "androidboot.selinux=permissive "
           "androidboot.boot_devices=a003e00.virtio "
           "androidboot.hardware.hwcomposer.display_finder_mode=drm "
           "loop.max_part=7 "
           "printk.devkmsg=on "
           "firmware_class.path=/vendor/firmware "
           "ro.adb.secure=0 "
           "ro.debuggable=1";
}

QString QemuConfig::findQemuBinary() {
#if defined(Q_PROCESSOR_ARM_64)
    QString target = "qemu-system-aarch64";
#else
    QString target = "qemu-system-x86_64";
#endif

    QString path = QStandardPaths::findExecutable(target);
    if (!path.isEmpty()) return path;

    QStringList extra_paths = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
    };
    path = QStandardPaths::findExecutable(target, extra_paths);
    if (!path.isEmpty()) return path;

    return target;
}

QemuConfig::Accelerator QemuConfig::detectAccelerator() {
#if defined(Q_OS_MACOS)
    return Accelerator::HVF;
#elif defined(Q_OS_LINUX)
    return Accelerator::KVM;
#elif defined(Q_OS_WIN)
    return Accelerator::WHPX;
#else
    return Accelerator::TCG;
#endif
}

void QemuConfig::generateSocketPaths(const QString& instance_id) {
    QString id = instance_id.isEmpty()
                     ? QUuid::createUuid().toString(QUuid::Id128).left(8)
                     : instance_id;
#ifdef Q_OS_WIN
    spice_socket_path = QString("rex-spice-%1").arg(id);
    qmp_socket_path = QString("rex-qmp-%1").arg(id);
#else
    QString tmp = QDir::tempPath();
    spice_socket_path = QString("%1/rex-spice-%2.sock").arg(tmp, id);
    qmp_socket_path = QString("%1/rex-qmp-%2.sock").arg(tmp, id);
#endif
}

} // namespace rex::qemu
