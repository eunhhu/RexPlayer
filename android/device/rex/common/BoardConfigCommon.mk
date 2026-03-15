# RexPlayer Common Board Configuration
# Shared between rex_x86_64 and rex_arm64 targets

# Kernel
BOARD_KERNEL_CMDLINE := \
    androidboot.hardware=rex \
    androidboot.console=ttyS0 \
    console=ttyS0 \
    androidboot.selinux=permissive \
    loop.max_part=7

# Partitions
TARGET_USERIMAGES_USE_EXT4 := true
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4

BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2147483648    # 2 GB
BOARD_VENDORIMAGE_PARTITION_SIZE := 268435456     # 256 MB
BOARD_USERDATAIMAGE_PARTITION_SIZE := 4294967296  # 4 GB
BOARD_CACHEIMAGE_PARTITION_SIZE := 268435456      # 256 MB
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4

# Filesystem
TARGET_COPY_OUT_VENDOR := vendor
TARGET_COPY_OUT_PRODUCT := product

# Graphics
USE_OPENGL_RENDERER := true
BOARD_GPU_DRIVERS := virgl

# Virtio
BOARD_HAVE_VIRTIO_BLK := true
BOARD_HAVE_VIRTIO_NET := true
BOARD_HAVE_VIRTIO_GPU := true
BOARD_HAVE_VIRTIO_INPUT := true
BOARD_HAVE_VIRTIO_VSOCK := true
BOARD_HAVE_VIRTIO_SND := true

# SELinux
BOARD_SEPOLICY_DIRS += device/rex/common/sepolicy

# Recovery
TARGET_NO_RECOVERY := true

# No hardware camera/sensors
BOARD_HAVE_CAMERA := false
