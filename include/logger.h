/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Logger - Production-grade logging with Emoji safety and lazy evaluation
 */

#ifndef ATP_LOGGER_H
#define ATP_LOGGER_H

#include <stdarg.h>
#include <pthread.h>
#include <sys/types.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

/* ========== Color Defines ========== */

#define COLOR_RESET     "\033[0m"
#define COLOR_BLACK     "\033[30m"
#define COLOR_RED       "\033[31m"
#define COLOR_GREEN     "\033[32m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_BLUE      "\033[34m"
#define COLOR_MAGENTA   "\033[35m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_WHITE     "\033[37m"
#define COLOR_BOLD      "\033[1m"
#define COLOR_DIM       "\033[2m"

/* ========== Types ========== */

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} log_level_t;

typedef enum {
    LOG_TARGET_STDERR   = 1 << 0,
    LOG_TARGET_FILE     = 1 << 1,
    LOG_TARGET_SYSLOG   = 1 << 2,
    LOG_TARGET_ANDROID  = 1 << 3
} log_target_t;

typedef struct {
    log_level_t min_level;
    int targets;
    char log_file[PATH_MAX];
    int enable_timestamp;
    int enable_color;
    size_t max_file_size;
    int rotate_count;
    pthread_mutex_t mutex;
} log_config_t;

/* ========== Public API ========== */

void logger_init(void);
void logger_close(void);
void log_init(void);
void log_set_level(log_level_t level);
void log_set_target(int targets);
void log_set_file(const char *path);
void log_set_color(int enable);
void log_rotate(void);

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/* ========== Standard Log Macros ========== */

#define LOG_DEBUG(fmt, ...) \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...) \
    log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

/* ========== P1: Lazy Evaluation Macros (Zero-cost when disabled) ========== */

#define LOG_DEBUG_LAZY(fmt, ...) do { \
    extern log_config_t g_log_config; \
    if (g_log_config.min_level <= LOG_LEVEL_DEBUG) { \
        log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_INFO_LAZY(fmt, ...) do { \
    extern log_config_t g_log_config; \
    if (g_log_config.min_level <= LOG_LEVEL_INFO) { \
        log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_WARN_LAZY(fmt, ...) do { \
    extern log_config_t g_log_config; \
    if (g_log_config.min_level <= LOG_LEVEL_WARN) { \
        log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* ========== P2: Module-specific Macros (Grep-friendly) ========== */

#define LOG_SERVICE(level, fmt, ...) \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[SERVICE] " fmt, ##__VA_ARGS__)

#define LOG_API(level, fmt, ...) \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[API] " fmt, ##__VA_ARGS__)

#define LOG_ROUTE(level, fmt, ...) \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[ROUTE] " fmt, ##__VA_ARGS__)

#define LOG_NETLINK(level, fmt, ...) \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[NETLINK] " fmt, ##__VA_ARGS__)

#define LOG_REACTOR(level, fmt, ...) \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[REACTOR] " fmt, ##__VA_ARGS__)

/* ========== Utility Macros ========== */

#define LOG_EXEC(cmd) LOG_DEBUG("[EXEC] %s", cmd)

#define LOG_MODULE(level, module, fmt, ...) \
    log_write(level, __FILE__, __LINE__, module, fmt, ##__VA_ARGS__)

#define LOG_SERVICE(level, fmt, ...) LOG_MODULE(level, "[SERVICE]", fmt, ##__VA_ARGS__)
#define LOG_API(level, fmt, ...)     LOG_MODULE(level, "[API]", fmt, ##__VA_ARGS__)
#define LOG_ROUTE(level, fmt, ...)   LOG_MODULE(level, "[ROUTE]", fmt, ##__VA_ARGS__)
#define LOG_NETLINK(level, fmt, ...) LOG_MODULE(level, "[NETLINK]", fmt, ##__VA_ARGS__)
#define LOG_REACTOR(level, fmt, ...) LOG_MODULE(level, "[REACTOR]", fmt, ##__VA_ARGS__)

#endif /* ATP_LOGGER_H */
