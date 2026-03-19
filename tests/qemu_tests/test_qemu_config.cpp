#include <gtest/gtest.h>
#include "../../src/qemu/qemu_config.h"

namespace {

TEST(QemuConfigTest, DefaultCommandLineContainsRequiredArgs) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/test-spice.sock";
    config.qmp_socket_path = "/tmp/test-qmp.sock";

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("-machine"));
    EXPECT_TRUE(args.contains("-cpu"));
    EXPECT_TRUE(args.contains("-smp"));
    EXPECT_TRUE(args.contains("-m"));
    EXPECT_TRUE(args.contains("-display"));
    EXPECT_TRUE(args.contains("none"));
    EXPECT_TRUE(args.contains("-vnc"));
    EXPECT_TRUE(args.contains("-qmp"));
}

TEST(QemuConfigTest, VcpuAndRamInArgs) {
    rex::qemu::QemuConfig config;
    config.vcpus = 8;
    config.ram_mb = 8192;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    int smp_idx = args.indexOf("-smp");
    ASSERT_GE(smp_idx, 0);
    EXPECT_EQ(args.at(smp_idx + 1), "8");

    int m_idx = args.indexOf("-m");
    ASSERT_GE(m_idx, 0);
    EXPECT_EQ(args.at(m_idx + 1), "8192");
}

TEST(QemuConfigTest, KernelBootArgs) {
    rex::qemu::QemuConfig config;
    config.kernel_path = "/path/to/Image";
    config.initrd_path = "/path/to/initrd.img";
    config.cmdline = "console=ttyAMA0";
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("-kernel"));
    EXPECT_TRUE(args.contains("/path/to/Image"));
    EXPECT_TRUE(args.contains("-initrd"));
    EXPECT_TRUE(args.contains("/path/to/initrd.img"));
    EXPECT_TRUE(args.contains("-append"));
    EXPECT_TRUE(args.contains("console=ttyAMA0"));
}

TEST(QemuConfigTest, NoKernelOmitsBootArgs) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    EXPECT_FALSE(args.contains("-kernel"));
    EXPECT_FALSE(args.contains("-initrd"));
    EXPECT_FALSE(args.contains("-append"));
}

TEST(QemuConfigTest, DevicesPresent) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("virtio-gpu-pci"));
    EXPECT_TRUE(args.contains("virtio-keyboard-pci"));
    EXPECT_TRUE(args.contains("virtio-tablet-pci"));
    EXPECT_TRUE(args.contains("intel-hda"));
    EXPECT_TRUE(args.contains("hda-duplex,audiodev=snd0"));
    EXPECT_TRUE(args.contains("virtio-net-pci,netdev=net0"));
}

TEST(QemuConfigTest, PortForwardingInNetdev) {
    rex::qemu::QemuConfig config;
    config.adb_host_port = 5555;
    config.adb_guest_port = 5555;
    config.frida_host_port = 27042;
    config.frida_guest_port = 27042;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";

    auto args = config.toCommandLine();

    int idx = args.indexOf("-netdev");
    ASSERT_GE(idx, 0);
    QString netdev = args.at(idx + 1);
    EXPECT_TRUE(netdev.contains("hostfwd=tcp::5555-:5555"));
    EXPECT_TRUE(netdev.contains("hostfwd=tcp::27042-:27042"));
}

TEST(QemuConfigTest, ExtraArgsAppended) {
    rex::qemu::QemuConfig config;
    config.spice_socket_path = "/tmp/s.sock";
    config.qmp_socket_path = "/tmp/q.sock";
    config.extra_args = {"-monitor", "stdio"};

    auto args = config.toCommandLine();

    EXPECT_TRUE(args.contains("-monitor"));
    EXPECT_TRUE(args.contains("stdio"));
}

TEST(QemuConfigTest, GenerateSocketPathsUnique) {
    rex::qemu::QemuConfig a, b;
    a.generateSocketPaths("");
    b.generateSocketPaths("");

    EXPECT_NE(a.spice_socket_path, b.spice_socket_path);
    EXPECT_NE(a.qmp_socket_path, b.qmp_socket_path);
}

TEST(QemuConfigTest, GenerateSocketPathsWithId) {
    rex::qemu::QemuConfig config;
    config.generateSocketPaths("test123");

    EXPECT_TRUE(config.spice_socket_path.contains("test123"));
    EXPECT_TRUE(config.qmp_socket_path.contains("test123"));
}

TEST(QemuConfigTest, AcceleratorDetection) {
    auto accel = rex::qemu::QemuConfig::detectAccelerator();
#if defined(Q_OS_MACOS)
    EXPECT_EQ(accel, rex::qemu::QemuConfig::Accelerator::HVF);
#elif defined(Q_OS_LINUX)
    EXPECT_EQ(accel, rex::qemu::QemuConfig::Accelerator::KVM);
#elif defined(Q_OS_WIN)
    EXPECT_EQ(accel, rex::qemu::QemuConfig::Accelerator::WHPX);
#endif
}

} // namespace
