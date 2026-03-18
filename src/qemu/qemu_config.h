#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

namespace rex::qemu {

struct QemuConfig {
    uint32_t vcpus = 4;
    uint32_t ram_mb = 4096;
    QString machine = "virt,gic-version=3";
    QString cpu = "cortex-a76";

    QString kernel_path;
    QString initrd_path;
    QString cmdline;
    QString system_image_path;

    uint32_t display_width = 1080;
    uint32_t display_height = 1920;

    QString qemu_binary;
    QString spice_socket_path;
    QString qmp_socket_path;

    uint16_t adb_host_port = 5555;
    uint16_t adb_guest_port = 5555;

    enum class Accelerator { Auto, HVF, KVM, WHPX, TCG };
    Accelerator accelerator = Accelerator::Auto;

    uint16_t frida_host_port = 27042;
    uint16_t frida_guest_port = 27042;

    QStringList extra_args;

    QStringList toCommandLine() const;
    static QString findQemuBinary();
    static Accelerator detectAccelerator();
    void generateSocketPaths(const QString& instance_id);
};

} // namespace rex::qemu
