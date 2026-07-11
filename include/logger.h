#include <stdint.h>
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

/* ========== Color Macros ========== */
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

#ifndef LOG_LOCATION_ENABLED
#define LOG_LOCATION_ENABLED 1
#endif

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4,
    LOG_LEVEL_NONE = 5
} log_level_t;

typedef enum {
    LOG_TARGET_STDERR = 1 << 0,
    LOG_TARGET_FILE   = 1 << 1,
    LOG_TARGET_SYSLOG = 1 << 2,
} log_target_t;

typedef struct {
    log_level_t min_level;
    char log_file[256];
    uint32_t targets;
    int enable_color;
    int enable_timestamp;
    size_t max_file_size;
    int rotate_count;
    pthread_mutex_t mutex;
} log_config_t;

#ifdef __cplusplus
extern "C" {
#endif

void log_init(void);
void log_set_level(log_level_t level);
void log_set_file(const char *path);
void log_set_targets(uint32_t targets);
void log_set_color(int enable);
log_level_t log_get_level(void);
void log_rotate(void);
void logger_init(void);
void logger_close(void);
void log_set_target(int targets);

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void log_write_v(log_level_t level, const char *file, int line, const char *func,
                 const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#define LOG_DEBUG(fmt, ...) do { \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ERROR(fmt, ...) do { \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_FATAL(fmt, ...) do { \
    log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_SERVICE(level, fmt, ...) do { \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[SERVICE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_API(level, fmt, ...) do { \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[API] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ROUTE(level, fmt, ...) do { \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[ROUTE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_NETLINK(level, fmt, ...) do { \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[NETLINK] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_REACTOR(level, fmt, ...) do { \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[REACTOR] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_EBPF(level, fmt, ...) do { \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[EBPF] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_EXEC(cmd) do { \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, "EXEC: %s", cmd); \
} while(0)

#define LOG_DEBUG_LAZY(fmt, ...) LOG_DEBUG(fmt, ##__VA_ARGS__)
#define LOG_INFO_LAZY(fmt, ...)  LOG_INFO(fmt, ##__VA_ARGS__)
#define LOG_WARN_LAZY(fmt, ...)  LOG_WARN(fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
