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
