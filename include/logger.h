#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#ifdef __ANDROID__
#include <android/log.h>
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#endif

#define LOG_TAG "atpd"

/* ========== Log Levels ========== */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
    LOG_LEVEL_NONE = 5
} log_level_t;

/* ========== Log Targets ========== */
typedef enum {
    LOG_TARGET_STDERR = 1 << 0,
    LOG_TARGET_FILE   = 1 << 1,
    LOG_TARGET_SYSLOG = 1 << 2,
} log_target_t;

/* ========== Log Configuration ========== */
typedef struct {
    log_level_t min_level;
    char log_file[256];
    uint32_t targets;
    int enable_color;
    pthread_mutex_t mutex;
} log_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Core Log Functions ========== */
void log_init(void);
void log_set_level(log_level_t level);
void log_set_file(const char *path);
void log_set_targets(uint32_t targets);
void log_set_color(int enable);
log_level_t log_get_level(void);

void log_write(log_level_t level, const char *fmt, ...);
void log_write_v(log_level_t level, const char *fmt, va_list args);

/* ========== Convenience Macros ========== */

#ifndef LOG_LOCATION_ENABLED
#define LOG_LOCATION_ENABLED 1
#endif

#if LOG_LOCATION_ENABLED

#define LOG_DEBUG(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[DEBUG] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_INFO, LOG_TAG, "[INFO] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_WARN, LOG_TAG, "[WARN] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ERROR(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[ERROR] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_FATAL(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_FATAL, LOG_TAG, "[FATAL] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_SERVICE(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[SERVICE] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[SERVICE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_API(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[API] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[API] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ROUTE(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[ROUTE] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[ROUTE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_NETLINK(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[NETLINK] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[NETLINK] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_REACTOR(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[REACTOR] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[REACTOR] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_EBPF(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[EBPF] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[EBPF] " fmt, ##__VA_ARGS__); \
} while(0)

#else

#define LOG_DEBUG(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[DEBUG] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_INFO, LOG_TAG, "[INFO] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_WARN, LOG_TAG, "[WARN] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ERROR(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[ERROR] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_FATAL(fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(ANDROID_LOG_FATAL, LOG_TAG, "[FATAL] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_SERVICE(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[SERVICE] " fmt, ##__VA_ARGS__); \
    log_write(level, "[SERVICE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_API(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[API] " fmt, ##__VA_ARGS__); \
    log_write(level, "[API] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ROUTE(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[ROUTE] " fmt, ##__VA_ARGS__); \
    log_write(level, "[ROUTE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_NETLINK(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[NETLINK] " fmt, ##__VA_ARGS__); \
    log_write(level, "[NETLINK] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_REACTOR(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[REACTOR] " fmt, ##__VA_ARGS__); \
    log_write(level, "[REACTOR] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_EBPF(level, fmt, ...) do { \
    atpd_log_init(); \
    if (atpd_log_print) atpd_log_print(level, LOG_TAG, "[EBPF] " fmt, ##__VA_ARGS__); \
    log_write(level, "[EBPF] " fmt, ##__VA_ARGS__); \
} while(0)

#endif

/* ========== Lazy Evaluation Macros ========== */
#define LOG_DEBUG_LAZY(fmt, ...) LOG_DEBUG(fmt, ##__VA_ARGS__)
#define LOG_INFO_LAZY(fmt, ...)  LOG_INFO(fmt, ##__VA_ARGS__)
#define LOG_WARN_LAZY(fmt, ...)  LOG_WARN(fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOGGER_H */
