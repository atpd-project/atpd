# Android.mk for NDK build
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := atpd
LOCAL_SRC_FILES := \
    ../src/main.c \
    ../src/cli.c \
    ../src/logger.c \
    ../src/config.c \
    ../src/tproxy.c \
    ../src/routing.c \
    ../src/ipset.c \
    ../src/netlink.c \
    ../src/service.c \
    ../src/geoip.c \
    ../src/utils.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include

LOCAL_CFLAGS := -Wall -Wextra -O2 -D__ANDROID__ -D_GNU_SOURCE
LOCAL_LDLIBS := -llog -lcurl

include $(BUILD_EXECUTABLE)