/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Logger - Android logcat + file dual output
 */

#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define LOG_TAG "atpd"
#define MAX_LOG_MSG 4096

static log_level_t g_log_level = LOG_LEVEL_INFO;
static int g_log_color = 1;
static int g_log_timestamp = 1;
static int g_log_fd = -1;
static int g_log_fallback = 0;

static const char *level_strings[] = {
    [LOG_LEVEL_DEBUG] = "Debug",
    [LOG_LEVEL_INFO]  = "Info",
    [LOG_LEVEL_WARN]  = "Warn",
    [LOG_LEVEL_ERROR] = "Error"
};

#ifdef __ANDROID__
static int android_log_levels[] = {
    [LOG_LEVEL_DEBUG] = ANDROID_LOG_DEBUG,
    [LOG_LEVEL_INFO]  = ANDROID_LOG_INFO,
    [LOG_LEVEL_WARN]  = ANDROID_LOG_WARN,
    [LOG_LEVEL_ERROR] = ANDROID_LOG_ERROR
};
#endif

void logger_init(void) {
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s", ATP_DEFAULT_DIR, ATP_LOG_FILE);

    mkdir_recursive(ATP_DEFAULT_DIR "/run", 0755);

    g_log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (g_log_fd < 0) {
        g_log_fallback = 1;
    }
}

void logger_close(void) {
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

void log_set_level(log_level_t level) {
    g_log_level = level;
}

void log_set_color(int enable) {
    g_log_color = enable;
}

void log_set_timestamp(int enable) {
    g_log_timestamp = enable;
}

static const char *get_timestamp(void) {
    static char buf[32];
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }
    return "0000-00-00 00:00:00";
}

void log_write_msg(log_level_t level, const char *file, int line, const char *fmt, ...) {
    if (level < g_log_level) return;

    char msg[MAX_LOG_MSG];
    char full_msg[MAX_LOG_MSG + 128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const char *ts = get_timestamp();

    if (g_log_timestamp) {
        snprintf(full_msg, sizeof(full_msg), "[%s] %s %s:%d: %s",
                 ts, level_strings[level], file, line, msg);
    } else {
        snprintf(full_msg, sizeof(full_msg), "%s %s:%d: %s",
                 level_strings[level], file, line, msg);
    }

    size_t len = strlen(full_msg);
    if (len > 0 && full_msg[len-1] != '\n') {
        full_msg[len] = '\n';
        full_msg[len+1] = '\0';
    }

#ifdef __ANDROID__
    __android_log_write(android_log_levels[level], LOG_TAG, msg);
#endif

    if (!g_log_fallback && g_log_fd >= 0) {
        ssize_t ret = write(g_log_fd, full_msg, strlen(full_msg));
        if (ret < 0) {
            if (errno == ENOSPC || errno == ENOMEM) {
                g_log_fallback = 1;
                close(g_log_fd);
                g_log_fd = -1;
            }
        }
    }
}

log_level_t log_get_level(void) {
    return g_log_level;
}
