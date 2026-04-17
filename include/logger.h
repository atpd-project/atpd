#ifndef ATP_LOGGER_H
#define ATP_LOGGER_H

#include <stdarg.h>
#include <pthread.h>
#include <sys/types.h>
#include <limits.h>

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

void log_init(void);
void log_set_level(log_level_t level);
void log_set_target(int targets);
void log_set_file(const char *path);
void log_set_color(int enable);
void log_rotate(void);
void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) __attribute__((format(printf, 5, 6)));

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

#define LOG_EXEC(cmd) LOG_DEBUG("[EXEC] %s", cmd)

#endif
