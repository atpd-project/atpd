# Android.mk for NDK build
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := atpd
LOCAL_SRC_FILES := \
    ../src/main.c \
    ../src/cli.c \
    ../src/logger.c \
    ../src/config.c \
    ../src/netlink.c \
    ../src/ebpf.c \
    ../src/ebpf_common.c \
    ../src/reactor.c \
    ../src/service.c \
    ../src/session.c \
    ../src/splice.c \
    ../src/singbox_api.c \
    ../src/api.c \
    ../src/async_validate.c \
    ../src/status.c \
    ../src/uds.c \
    ../src/ui.c \
    ../src/utils.c \
    ../src/version.c \
    ../src/atpd_context.c \
    ../src/atpd_error.c \
    ../src/atpd_global.c \
    ../src/atpd_init.c \
    ../src/cleanup.c \
    ../src/yyjson.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include

LOCAL_CFLAGS := -Wall -Wextra -O2 -D__ANDROID__ -D_GNU_SOURCE
LOCAL_LDLIBS := -llog

include $(BUILD_EXECUTABLE)
