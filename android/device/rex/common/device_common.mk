# RexPlayer Common Device Configuration
# Shared between x86_64 and ARM64 targets

# Display
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=320 \
    ro.hardware.egl=mesa

# Virtio drivers (built into kernel, userspace components)
PRODUCT_PACKAGES += \
    virtio_gpu \
    virtio_input

# Audio
PRODUCT_PACKAGES += \
    audio.primary.rex \
    audio.r_submix.default

# Frida Server (integrated)
PRODUCT_PACKAGES += \
    frida-server

PRODUCT_COPY_FILES += \
    device/rex/common/init.rex.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.rex.rc

# Networking
PRODUCT_PACKAGES += \
    netd \
    dhcpclient

PRODUCT_PROPERTY_OVERRIDES += \
    net.eth0.dns1=8.8.8.8 \
    net.eth0.dns2=8.8.4.4

# USB / ADB
PRODUCT_PROPERTY_OVERRIDES += \
    persist.sys.usb.config=adb \
    ro.adb.secure=0 \
    ro.debuggable=1 \
    service.adb.root=1

# Performance
PRODUCT_PROPERTY_OVERRIDES += \
    dalvik.vm.heapsize=512m \
    dalvik.vm.heapgrowthlimit=256m \
    dalvik.vm.heapstartsize=8m \
    dalvik.vm.heapminfree=512k \
    dalvik.vm.heapmaxfree=8m

# Security (permissive for development)
PRODUCT_PROPERTY_OVERRIDES += \
    ro.secure=0 \
    security.perf_harden=0
