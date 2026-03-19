#include "qemu_config.h"
#include <QDir>
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

    if (!system_image_path.isEmpty()) {
        args << "-drive"
             << QString("file=%1,format=raw,if=virtio").arg(system_image_path);
    }

    if (!kernel_path.isEmpty()) {
        args << "-kernel" << kernel_path;
    }
    if (!initrd_path.isEmpty()) {
        args << "-initrd" << initrd_path;
    }
    if (!cmdline.isEmpty()) {
        args << "-append" << cmdline;
    }

    args << "-display" << "none";

    // Display backend: VNC (universally supported) or SPICE (optional)
    DisplayBackend db = display_backend;
    if (db == DisplayBackend::Auto) {
        // Auto-detect: try SPICE socket path presence as hint, otherwise VNC
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
        // VNC on localhost
        args << "-vnc" << QString("127.0.0.1:%1").arg(vnc_port - 5900);
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

    args << "-device" << "virtio-gpu-pci";
    args << "-device" << "virtio-keyboard-pci";
    args << "-device" << "virtio-tablet-pci";
    args << "-device" << "intel-hda" << "-device" << "hda-duplex";

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
