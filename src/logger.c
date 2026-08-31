/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Logger - Production-grade high-performance logging implementation
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

#define MAX_LOG_MSG 1024

#ifndef LOG_LOCATION_ENABLED
#define LOG_LOCATION_ENABLED 0
#endif

static FILE *g_log_fp = NULL;
static size_t g_current_log_size = 0;

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
static int atpd_log_initialized = 0;

static void atpd_log_init(void) {
    if (atpd_log_initialized) return;
    atpd_log_initialized = 1;
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

static void get_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) {
        strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(buf, size, "0000-00-00 00:00:00");
    }
}

static __attribute__((unused)) const char *get_file_basename(const char *path) {
    if (!path) return "unknown";
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static void log_close_file_unlocked(void) {
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    g_current_log_size = 0;
}

static void log_open_file_unlocked(void) {
    if (!g_log_config.log_file[0]) return;
    if (g_log_fp) return;

    int fd = open(g_log_config.log_file,
                  O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        return;
    }

    g_log_fp = fdopen(fd, "a");
    if (!g_log_fp) {
        close(fd);
        return;
    }
    if (g_log_fp) {
        setvbuf(g_log_fp, NULL, _IOLBF, 0);
        g_current_log_size = (size_t)st.st_size;
    }
}

static void log_rotate_file_unlocked(const char *path) {
    if (!path || !path[0]) return;

    log_close_file_unlocked();

    char old_path[PATH_MAX];
    char new_path[PATH_MAX];

    for (int i = g_log_config.rotate_count - 1; i > 0; i--) {
        if (i == 1) {
            snprintf(new_path, sizeof(new_path), "%s.1", path);
            rename(path, new_path);
        } else {
            snprintf(old_path, sizeof(old_path), "%s.%d", path, i - 1);
            snprintf(new_path, sizeof(new_path), "%s.%d", path, i);
            if (access(old_path, F_OK) == 0) {
                rename(old_path, new_path);
            }
        }
    }

    log_open_file_unlocked();
}

void log_init(void) {
    atp_timezone_init();

    char log_path[PATH_MAX + 32];
    char app_dir[PATH_MAX];
    get_app_dir(app_dir, sizeof(app_dir));
    snprintf(log_path, sizeof(log_path), "%s/%s", app_dir, ATP_LOG_FILE);

    char run_dir[PATH_MAX + 32];
    snprintf(run_dir, sizeof(run_dir), "%s/run", app_dir);
    mkdir_recursive(run_dir, 0755);

    pthread_mutex_lock(&g_log_config.mutex);
    strncpy(g_log_config.log_file, log_path, sizeof(g_log_config.log_file) - 1);
    g_log_config.log_file[sizeof(g_log_config.log_file) - 1] = '\0';
    log_close_file_unlocked();
    log_open_file_unlocked();
    pthread_mutex_unlock(&g_log_config.mutex);
}

void logger_init(void) {
    log_init();
}

void logger_close(void) {
    pthread_mutex_lock(&g_log_config.mutex);
    log_close_file_unlocked();
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_level(log_level_t level) {
    pthread_mutex_lock(&g_log_config.mutex);
    g_log_config.min_level = level;
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_target(int targets) {
    pthread_mutex_lock(&g_log_config.mutex);
    g_log_config.targets = targets;
    if ((targets & LOG_TARGET_FILE) && !g_log_fp && g_log_config.log_file[0]) {
        log_open_file_unlocked();
    } else if (!(targets & LOG_TARGET_FILE) && g_log_fp) {
        log_close_file_unlocked();
    }
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_targets(uint32_t targets) {
    log_set_target((int)targets);
}

void log_set_file(const char *path) {
    pthread_mutex_lock(&g_log_config.mutex);
    if (path) {
        strncpy(g_log_config.log_file, path, sizeof(g_log_config.log_file) - 1);
        g_log_config.log_file[sizeof(g_log_config.log_file) - 1] = '\0';
        log_close_file_unlocked();
        if (g_log_config.targets & LOG_TARGET_FILE) {
            log_open_file_unlocked();
        }
    }
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_color(int enable) {
    pthread_mutex_lock(&g_log_config.mutex);
    g_log_config.enable_color = enable;
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_rotate(void) {
    pthread_mutex_lock(&g_log_config.mutex);
    if (g_log_config.log_file[0]) {
        log_rotate_file_unlocked(g_log_config.log_file);
    }
    pthread_mutex_unlock(&g_log_config.mutex);
}

static void log_write_file_unlocked(log_level_t level, const char *file, int line,
                                   const char *func, const char *msg, const char *ts) {
    if (!g_log_fp) {
        log_open_file_unlocked();
        if (!g_log_fp) return;
    }

    int written = 0;
#if LOG_LOCATION_ENABLED
    written = fprintf(g_log_fp, "[%s] %s %s:%d %s: %s\n",
                      ts, level_strings[level], get_file_basename(file), line, func, msg);
#else
    (void)file; (void)line; (void)func;
    written = fprintf(g_log_fp, "[%s] %s: %s\n",
                      ts, level_strings[level], msg);
#endif

    if (written > 0) {
        g_current_log_size += (size_t)written;
        if (g_current_log_size >= g_log_config.max_file_size) {
            log_rotate_file_unlocked(g_log_config.log_file);
        }
    }
}

void log_write_v(log_level_t level, const char *file, int line, const char *func,
                 const char *fmt, va_list args) {
    if (level < g_log_config.min_level) return;

    char msg[MAX_LOG_MSG];
    vsnprintf(msg, sizeof(msg), fmt, args);

#ifdef __ANDROID__
    atpd_log_init();
    if (atpd_log_print) {
        atpd_log_print(android_log_level(level), LOG_TAG, "[%s] %s", level_strings[level], msg);
    }
#endif

    char ts[32];
    get_timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&g_log_config.mutex);

    if (g_log_config.targets & LOG_TARGET_FILE) {
        log_write_file_unlocked(level, file, line, func, msg, ts);
    }

    if (g_log_config.targets & LOG_TARGET_STDERR) {
        if (g_log_config.enable_color) {
#if LOG_LOCATION_ENABLED
            fprintf(stderr, "[%s] %s%s%s %s:%d %s: %s\n",
                    ts, level_colors[level], level_strings[level], COLOR_RESET,
                    get_file_basename(file), line, func, msg);
#else
            fprintf(stderr, "[%s] %s%s%s: %s\n",
                    ts, level_colors[level], level_strings[level], COLOR_RESET, msg);
#endif
        } else {
#if LOG_LOCATION_ENABLED
            fprintf(stderr, "[%s] %s %s:%d %s: %s\n",
                    ts, level_strings[level], get_file_basename(file), line, func, msg);
#else
            fprintf(stderr, "[%s] %s: %s\n",
                    ts, level_strings[level], msg);
#endif
        }
    }

    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    if (level < g_log_config.min_level) return;

    va_list args;
    va_start(args, fmt);
    log_write_v(level, file, line, func, fmt, args);
    va_end(args);
}

log_level_t log_get_level(void) {
    return g_log_config.min_level;
}
