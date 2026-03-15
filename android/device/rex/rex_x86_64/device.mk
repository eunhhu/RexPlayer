# RexPlayer x86_64 Device Configuration

PRODUCT_NAME := rex_x86_64
PRODUCT_DEVICE := rex_x86_64
PRODUCT_BRAND := RexPlayer
PRODUCT_MODEL := RexPlayer x86_64
PRODUCT_MANUFACTURER := RexPlayer

# Inherit common device config
$(call inherit-product, device/rex/common/device_common.mk)

# Inherit full AOSP
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_x86_64.mk)

# Architecture-specific packages
PRODUCT_PACKAGES += \
    gralloc.rex \
    hwcomposer.rex

# Native bridge (libhoudini for ARM app support)
PRODUCT_PACKAGES += \
    libhoudini

PRODUCT_PROPERTY_OVERRIDES += \
    ro.product.cpu.abilist=x86_64,x86,arm64-v8a,armeabi-v7a,armeabi \
    ro.product.cpu.abilist32=x86,armeabi-v7a,armeabi \
    ro.product.cpu.abilist64=x86_64,arm64-v8a \
    ro.dalvik.vm.native.bridge=libhoudini.so \
    ro.enable.native.bridge.exec=1

# Fstab
PRODUCT_COPY_FILES += \
    device/rex/rex_x86_64/fstab.rex:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.rex
