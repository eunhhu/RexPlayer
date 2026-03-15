# RexPlayer x86_64 Board Configuration

include device/rex/common/BoardConfigCommon.mk

# Architecture
TARGET_ARCH := x86_64
TARGET_ARCH_VARIANT := x86_64
TARGET_CPU_ABI := x86_64
TARGET_CPU_ABI2 :=
TARGET_2ND_ARCH := x86
TARGET_2ND_ARCH_VARIANT := x86_64
TARGET_2ND_CPU_ABI := x86

# Kernel
TARGET_KERNEL_ARCH := x86_64
BOARD_KERNEL_IMAGE_NAME := bzImage
TARGET_PREBUILT_KERNEL := device/rex/rex_x86_64/kernel/bzImage

# Boot
TARGET_NO_BOOTLOADER := true

# Native Bridge (ARM translation)
ENABLE_NATIVEBRIDGE_64BIT := true
WITH_NATIVE_BRIDGE := true

# Disk
BOARD_FLASH_BLOCK_SIZE := 512
