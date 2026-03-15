# RexPlayer ARM64 Device Configuration

PRODUCT_NAME := rex_arm64
PRODUCT_DEVICE := rex_arm64
PRODUCT_BRAND := RexPlayer
PRODUCT_MODEL := RexPlayer ARM64
PRODUCT_MANUFACTURER := RexPlayer

# Inherit common device config
$(call inherit-product, device/rex/common/device_common.mk)

# Inherit full AOSP
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_arm64.mk)

# Architecture-specific packages
PRODUCT_PACKAGES += \
    gralloc.rex \
    hwcomposer.rex

# No native bridge needed — ARM apps run natively on ARM64 host
PRODUCT_PROPERTY_OVERRIDES += \
    ro.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi \
    ro.product.cpu.abilist32=armeabi-v7a,armeabi \
    ro.product.cpu.abilist64=arm64-v8a

# Fstab
PRODUCT_COPY_FILES += \
    device/rex/rex_arm64/fstab.rex:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.rex
