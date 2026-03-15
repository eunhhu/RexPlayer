# RexPlayer ARM64 Board Configuration
# Used on Apple Silicon (HVF ARM64 native) and future ARM hosts

include device/rex/common/BoardConfigCommon.mk

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi

# Kernel
TARGET_KERNEL_ARCH := arm64
BOARD_KERNEL_IMAGE_NAME := Image
TARGET_PREBUILT_KERNEL := device/rex/rex_arm64/kernel/Image

# Boot
TARGET_NO_BOOTLOADER := true

# No native bridge needed — ARM apps run natively
WITH_NATIVE_BRIDGE := false

# Disk
BOARD_FLASH_BLOCK_SIZE := 512
