LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := drm-framebuffer
LOCAL_C_INCLUDES:=$(LOCAL_PATH)
LOCAL_SHARED_LIBRARIES := libdrm
LOCAL_SRC_FILES := main.c framebuffer.c
include $(BUILD_EXECUTABLE)
