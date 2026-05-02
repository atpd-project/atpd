/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Logger - Production-grade implementation
 * Features: UTF-8 safe truncation, persistent file handle, module tags, syslog
 */

#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <syslog.h>
#include <libgen.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define SAFE_PATH_MAX (PATH_MAX + 128)
#define LOG_MSG_MAX 4096

/* ========== Global Config ========== */

log_config_t g_log_config = {
    .min_level = LOG_LEVEL_INFO,
    .targets = LOG_TARGET_STDERR | LOG_TARGET_FILE,
    .log_file = "",
    .enable_timestamp = 1,
    .enable_color = 1,
    .max_file_size = 10 * 1024 * 1024,
    .rotate_count = 3,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

/* ========== Persistent File Handle ========== */

static FILE *g_log_fp = NULL;

static FILE* log_file_open(void) {
    if (g_log_fp) return g_log_fp;
    if (g_log_config.log_file[0] == '\0') return NULL;
    g_log_fp = fopen(g_log_config.log_file, "a");
    return g_log_fp;
}

static void log_file_reopen(void) {
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

/* ========== Level Names & Colors ========== */

static const char *level_names[] = {
    [LOG_LEVEL_DEBUG] = "Debug",
    [LOG_LEVEL_INFO]  = "Info",
    [LOG_LEVEL_WARN]  = "Warn",
    [LOG_LEVEL_ERROR] = "Error",
    [LOG_LEVEL_FATAL] = "Fatal"
};

static const char *level_colors[] = {
    [LOG_LEVEL_DEBUG] = "\033[36m",
    [LOG_LEVEL_INFO]  = "\033[32m",
    [LOG_LEVEL_WARN]  = "\033[33m",
    [LOG_LEVEL_ERROR] = "\033[31m",
    [LOG_LEVEL_FATAL] = "\033[35m\033[1m"
};

/* ========== UTF-8 Safe Truncation ========== */

static size_t utf8_safe_truncate(const char *str, size_t max_len) {
    if (!str) return 0;
    
    size_t len = strlen(str);
    if (len <= max_len) return len;
    
    size_t pos = max_len;
    while (pos > 0) {
        unsigned char c = (unsigned char)str[pos];
        if ((c & 0xC0) != 0x80) {
            return pos;
        }
        pos--;
    }
    
    return 0;
}

/* ========== Timestamp ========== */

static void get_timestamp(char *buf, size_t size) {
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ========== File Rotation ========== */

static void log_rotate_if_needed(void) {
    struct stat st;
    if (g_log_config.max_file_size == 0 || g_log_config.log_file[0] == '\0') return;
    if (stat(g_log_config.log_file, &st) != 0 || (size_t)st.st_size < g_log_config.max_file_size) return;

    log_file_reopen();

    char old_p[SAFE_PATH_MAX], new_p[SAFE_PATH_MAX];
    size_t base_len = strlen(g_log_config.log_file);
    if (base_len >= PATH_MAX) return;

    for (int i = g_log_config.rotate_count - 1; i > 0; i--) {
        snprintf(old_p, sizeof(old_p), "%s.%d", g_log_config.log_file, i);
        snprintf(new_p, sizeof(new_p), "%s.%d", g_log_config.log_file, i + 1);
        rename(old_p, new_p);
    }
    snprintf(new_p, sizeof(new_p), "%s.1", g_log_config.log_file);
    rename(g_log_config.log_file, new_p);

    log_file_open();
}

/* ========== Syslog ========== */

static void log_to_syslog(log_level_t level, const char *msg) {
    if (!(g_log_config.targets & LOG_TARGET_SYSLOG)) return;
    static int opened = 0;
    if (!opened) { openlog("atp", LOG_PID | LOG_NDELAY, LOG_DAEMON); opened = 1; }
    int prio;
    switch (level) {
        case LOG_LEVEL_DEBUG: prio = LOG_DEBUG; break;
        case LOG_LEVEL_INFO:  prio = LOG_INFO;  break;
        case LOG_LEVEL_WARN:  prio = LOG_WARNING; break;
        case LOG_LEVEL_ERROR: prio = LOG_ERR;   break;
        case LOG_LEVEL_FATAL: prio = LOG_CRIT;  break;
        default: prio = LOG_NOTICE;
    }
    syslog(prio, "%s", msg);
}

/* ========== Core Write Function ========== */

void log_write(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...) {
#ifndef __ANDROID__
    if (level < g_log_config.min_level) return;

    char ts[64] = {0};
    char raw_msg[LOG_MSG_MAX];
    char safe_msg[LOG_MSG_MAX];

    if (g_log_config.enable_timestamp) get_timestamp(ts, sizeof(ts));

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw_msg, sizeof(raw_msg), fmt, ap);
    va_end(ap);

    size_t safe_len = utf8_safe_truncate(raw_msg, LOG_MSG_MAX - 1);
    memcpy(safe_msg, raw_msg, safe_len);
    safe_msg[safe_len] = '\0';

    pthread_mutex_lock(&g_log_config.mutex);

    /* Stderr output */
    if (g_log_config.targets & LOG_TARGET_STDERR) {
        if (g_log_config.enable_color && isatty(STDERR_FILENO))
            fprintf(stderr, "%s[%s]%s %s [%s]: %s\n",
                    level_colors[level], level_names[level], "\033[0m", func, ts, safe_msg);
        else
            fprintf(stderr, "[%s] %s [%s]: %s\n", ts, level_names[level], func, safe_msg);
    }

    /* File output with persistent handle */
    if ((g_log_config.targets & LOG_TARGET_FILE) && g_log_config.log_file[0]) {
        log_rotate_if_needed();
        FILE *fp = log_file_open();
        if (fp) {
            fprintf(fp, "[%s] %s [%s]: %s\n", ts, level_names[level], func, safe_msg);
            fflush(fp);
        }
    }

    pthread_mutex_unlock(&g_log_config.mutex);

    log_to_syslog(level, safe_msg);
#endif
}

/* ========== Public API ========== */

void logger_init(void) {
    if (g_log_config.log_file[0] == '\0') {
        if (g_config.data_dir[0]) {
            snprintf(g_log_config.log_file, sizeof(g_log_config.log_file), "%s/%s", g_config.data_dir, ATP_LOG_FILE);
        } else {
            snprintf(g_log_config.log_file, sizeof(g_log_config.log_file), "./atpd.log");
        }
    }
    char *tmp = strdup(g_log_config.log_file);
    if (tmp) { char *dir = dirname(tmp); mkdir_recursive(dir, 0755); free(tmp); }
}

void logger_close(void) {
    log_file_reopen();
}

void log_set_level(log_level_t l) { g_log_config.min_level = l; }
void log_set_target(int t) { g_log_config.targets = t; }
void log_set_color(int e) { g_log_config.enable_color = e; }

void log_rotate(void) {
    pthread_mutex_lock(&g_log_config.mutex);
    log_rotate_if_needed();
    pthread_mutex_unlock(&g_log_config.mutex);
}

void log_set_file(const char *p) {
    if (p && p[0]) {
        snprintf(g_log_config.log_file, sizeof(g_log_config.log_file), "%s", p);
    }
}
