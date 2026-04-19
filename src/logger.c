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

static log_config_t g_log_config = {
    .min_level = LOG_LEVEL_INFO,
    .targets = LOG_TARGET_STDERR | LOG_TARGET_FILE,
    .log_file = "",
    .enable_timestamp = 1,
    .enable_color = 1,
    .max_file_size = 10 * 1024 * 1024,
    .rotate_count = 3,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

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

static void get_timestamp(char *buf, size_t size, int with_ms) {
    struct timeval tv;
    struct tm tm;
    (void)with_ms;  // Suppress unused parameter warning
    
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    
    // Format: YYYY-MM-DD HH:MM:SS (same as atp.sh)
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void log_rotate_if_needed(void) {
    if (g_log_config.max_file_size == 0) return;
    if (!(g_log_config.targets & LOG_TARGET_FILE)) return;
    if (g_log_config.log_file[0] == '\0') return;
    
    struct stat st;
    if (stat(g_log_config.log_file, &st) != 0) return;
    if ((size_t)st.st_size < g_log_config.max_file_size) return;
    
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];
    
    for (int i = g_log_config.rotate_count - 1; i > 0; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", g_log_config.log_file, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", g_log_config.log_file, i + 1);
        rename(old_path, new_path);
    }
    
    snprintf(new_path, sizeof(new_path), "%s.1", g_log_config.log_file);
    rename(g_log_config.log_file, new_path);
}

static void log_to_file(log_level_t level, const char *timestamp,
                         const char *file, int line, const char *func,
                         const char *msg) {
    if (!(g_log_config.targets & LOG_TARGET_FILE)) return;
    if (g_log_config.log_file[0] == '\0') return;
    (void)file;
    (void)line;
    (void)func;
    
    pthread_mutex_lock(&g_log_config.mutex);
    
    char *dir = dirname(strdup(g_log_config.log_file));
    mkdir_recursive(dir, 0755);
    free(dir);
    
    FILE *fp = fopen(g_log_config.log_file, "a");
    if (fp) {
        fprintf(fp, "[%s] [%s]: %s\n",
                timestamp, level_names[level], msg);
        fclose(fp);
    }
    
    log_rotate_if_needed();
    
    pthread_mutex_unlock(&g_log_config.mutex);
}

static void log_to_stderr(log_level_t level, const char *timestamp,
                           const char *file, int line, const char *func,
                           const char *msg) {
    if (!(g_log_config.targets & LOG_TARGET_STDERR)) return;
    (void)file;
    (void)line;
    (void)func;
    
    pthread_mutex_lock(&g_log_config.mutex);
    
    int use_color = g_log_config.enable_color && isatty(STDERR_FILENO);
    
    if (use_color) {
        fprintf(stderr, "%s[%s]%s [%s]: %s\n",
                level_colors[level], level_names[level], COLOR_RESET,
                timestamp, msg);
    } else {
        fprintf(stderr, "[%s] [%s]: %s\n",
                timestamp, level_names[level], msg);
    }
    
    pthread_mutex_unlock(&g_log_config.mutex);
}

static void log_to_syslog(log_level_t level, const char *msg) {
    if (!(g_log_config.targets & LOG_TARGET_SYSLOG)) return;
    
    static int syslog_opened = 0;
    if (!syslog_opened) {
        openlog("atp", LOG_PID | LOG_NDELAY, LOG_DAEMON);
        syslog_opened = 1;
    }
    
    int syslog_level;
    switch (level) {
        case LOG_LEVEL_DEBUG: syslog_level = LOG_DEBUG; break;
        case LOG_LEVEL_INFO:  syslog_level = LOG_INFO; break;
        case LOG_LEVEL_WARN:  syslog_level = LOG_WARNING; break;
        case LOG_LEVEL_ERROR: syslog_level = LOG_ERR; break;
        case LOG_LEVEL_FATAL: syslog_level = LOG_CRIT; break;
        default: syslog_level = LOG_NOTICE;
    }
    
    syslog(syslog_level, "%s", msg);
}

void log_write(log_level_t level, const char *file, int line, const char *func,
                const char *fmt, ...) {
    if (level < g_log_config.min_level) return;
    
    char timestamp[64] = {0};
    if (g_log_config.enable_timestamp) {
        get_timestamp(timestamp, sizeof(timestamp), 0);
    }
    
    char msg_buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buffer, sizeof(msg_buffer), fmt, args);
    va_end(args);
    
    log_to_stderr(level, timestamp, file, line, func, msg_buffer);
    log_to_file(level, timestamp, file, line, func, msg_buffer);
    log_to_syslog(level, msg_buffer);
}

void log_init(void) {
    char default_log_path[PATH_MAX];
    snprintf(default_log_path, sizeof(default_log_path), "%s/%s", 
             ATP_DEFAULT_DIR, ATP_LOG_FILE);
    
    if (g_log_config.log_file[0] == '\0') {
        snprintf(g_log_config.log_file, sizeof(g_log_config.log_file), "%s", default_log_path);
    }
    
    char *dir = dirname(strdup(g_log_config.log_file));
    mkdir_recursive(dir, 0755);
    free(dir);
    
    fprintf(stderr, "Logging initialized (level=%s, file=%s)\n", 
            level_names[g_log_config.min_level], g_log_config.log_file);
}

void log_set_level(log_level_t level) {
    g_log_config.min_level = level;
}

void log_set_target(int targets) {
    g_log_config.targets = targets;
}

void log_set_file(const char *path) {
    if (path && path[0]) {
        snprintf(g_log_config.log_file, sizeof(g_log_config.log_file), "%s", path);
    }
}

void log_set_color(int enable) {
    g_log_config.enable_color = enable;
}

void log_rotate(void) {
    log_rotate_if_needed();
}
void logger_init(void) {
    log_init();
}

void logger_close(void) {
    /* Close syslog if opened */
    /* No explicit cleanup needed for file logging */
}
