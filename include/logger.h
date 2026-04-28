/* ========== Android Logcat Integration ========== */
#ifdef __ANDROID__
#include <android/log.h>

#define LOG_TAG "atpd"

/* Android: Logcat + file dual output */
#define LOG_DEBUG(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[DEBUG] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_INFO(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[INFO] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "[WARN] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ERROR(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[ERROR] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_FATAL(fmt, ...) do { \
    __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, "[FATAL] " fmt, ##__VA_ARGS__); \
    log_write(LOG_LEVEL_FATAL, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_DEBUG_LAZY(fmt, ...) LOG_DEBUG(fmt, ##__VA_ARGS__)
#define LOG_INFO_LAZY(fmt, ...)  LOG_INFO(fmt, ##__VA_ARGS__)
#define LOG_WARN_LAZY(fmt, ...)  LOG_WARN(fmt, ##__VA_ARGS__)

#define LOG_SERVICE(level, fmt, ...) do { \
    __android_log_print(level, LOG_TAG, "[SERVICE] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[SERVICE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_API(level, fmt, ...) do { \
    __android_log_print(level, LOG_TAG, "[API] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[API] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_ROUTE(level, fmt, ...) do { \
    __android_log_print(level, LOG_TAG, "[ROUTE] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[ROUTE] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_NETLINK(level, fmt, ...) do { \
    __android_log_print(level, LOG_TAG, "[NETLINK] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[NETLINK] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_REACTOR(level, fmt, ...) do { \
    __android_log_print(level, LOG_TAG, "[REACTOR] " fmt, ##__VA_ARGS__); \
    log_write(level, __FILE__, __LINE__, __FUNCTION__, "[REACTOR] " fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_EXEC(cmd) LOG_DEBUG("[EXEC] %s", cmd)

/* Android: log_write available for dual output */
#else /* ========== Desktop/Linux Logging ========== */
