# Frida Server prebuilt installation
# The binary is fetched by scripts/fetch_frida.sh and placed here

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := frida-server
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT)/bin
LOCAL_SRC_FILES := frida-server-$(TARGET_ARCH)

# Frida needs to run as root
LOCAL_MODULE_OWNER := root
include $(BUILD_PREBUILT)
