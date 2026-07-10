/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Logger - Production-grade logging implementation
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
#include <dlfcn.h>
#endif

#define MAX_LOG_MSG 4096

log_config_t g_log_config = {
    .min_level = LOG_LEVEL_INFO,
    .targets = LOG_TARGET_STDERR | LOG_TARGET_FILE,
    .log_file = "",
    .enable_timestamp = 1,
    .enable_color = 1,
    .max_file_size = 10 * 1024 * 1024,
    .rotate_count = 5,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

#ifdef __ANDROID__
static int (*atpd_log_print)(int prio, const char *tag, const char *fmt, ...) = NULL;

static void atpd_log_init(void) {
    if (atpd_log_print) return;
    void *handle = dlopen("liblog.so", RTLD_LAZY);
    if (handle) {
        atpd_log_print = (int (*)(int, const char *, const char *, ...))
            dlsym(handle, "__android_log_print");
    }
}

static int android_log_level(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return ANDROID_LOG_DEBUG;
        case LOG_LEVEL_INFO:  return ANDROID_LOG_INFO;
        case LOG_LEVEL_WARN:  return ANDROID_LOG_WARN;
        case LOG_LEVEL_ERROR: return ANDROID_LOG_ERROR;
        case LOG_LEVEL_FATAL: return ANDROID_LOG_FATAL;
        default:              return ANDROID_LOG_INFO;
    }
}
#endif

static const char *level_strings[] = {
    [LOG_LEVEL_DEBUG] = "DEBUG",
    [LOG_LEVEL_INFO]  = "INFO",
    [LOG_LEVEL_WARN]  = "WARN",
    [LOG_LEVEL_ERROR] = "ERROR",
    [LOG_LEVEL_FATAL] = "FATAL"
};

static const char *level_colors[] = {
    [LOG_LEVEL_DEBUG] = COLOR_CYAN,
    [LOG_LEVEL_INFO]  = COLOR_GREEN,
    [LOG_LEVEL_WARN]  = COLOR_YELLOW,
    [LOG_LEVEL_ERROR] = COLOR_RED,
    [LOG_LEVEL_FATAL] = COLOR_RED COLOR_BOLD
};

void logger_init(void) {
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s", ATP_DEFAULT_DIR, ATP_LOG_FILE);

    mkdir_recursive(ATP_DEFAULT_DIR "/run", 0755);

    pthread_mutex_lock(&g_log_config.mutex);
    strncpy(g_log_config.log_file, log_path, sizeof(g_log_config.log_file) - 1);
    pthread_mutex_unlock(&g_log_config.mutex);
}

void logger_close(void) {
}

void log_set_level(log_level_t level) {
    pthread_mutex_lock(&g_log_config.mutex);
    g_log_config.min_level = level;
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_target(int targets) {
    pthread_mutex_lock(&g_log_config.mutex);
    g_log_config.targets = targets;
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_file(const char *path) {
    pthread_mutex_lock(&g_log_config.mutex);
    if (path) {
        strncpy(g_log_config.log_file, path, sizeof(g_log_config.log_file) - 1);
    }
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_color(int enable) {
    pthread_mutex_lock(&g_log_config.mutex);
    g_log_config.enable_color = enable;
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_rotate(void) {
    /* TODO: implement log rotation */
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

static void log_write_file(log_level_t level, const char *file, int line,
                           const char *func, const char *msg) {
    (void)level;
    FILE *fp = fopen(g_log_config.log_file, "a");
    if (!fp) return;

    const char *ts = get_timestamp();
    fprintf(fp, "[%s] %s %s:%d %s: %s\n",
            ts, level_strings[level], file, line, func, msg);
    fclose(fp);
}

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    if (level < g_log_config.min_level) return;

    char msg[MAX_LOG_MSG];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

#ifdef __ANDROID__
    atpd_log_init();
    if (atpd_log_print) {
        atpd_log_print(android_log_level(level), "atpd", "[%s] %s", level_strings[level], msg);
    }
#endif

    if (g_log_config.targets & LOG_TARGET_FILE) {
        log_write_file(level, file, line, func, msg);
    }

    if (g_log_config.targets & LOG_TARGET_STDERR) {
        const char *ts = get_timestamp();
        if (g_log_config.enable_color) {
            fprintf(stderr, "[%s] %s%s%s %s:%d %s: %s\n",
                    ts, level_colors[level], level_strings[level], COLOR_RESET,
                    file, line, func, msg);
        } else {
            fprintf(stderr, "[%s] %s %s:%d %s: %s\n",
                    ts, level_strings[level], file, line, func, msg);
        }
    }

    if (g_log_config.targets & LOG_TARGET_SYSLOG) {
        /* TODO: implement syslog */
    }
}

log_level_t log_get_level(void) {
    return g_log_config.min_level;
}
