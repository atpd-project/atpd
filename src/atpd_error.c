#include "atpd_error.h"
#include "logger.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

static atpd_error_ring_t g_error_ring = {
    .count = 0,
    .head = 0,
    .tail = 0,
    .total_count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

static const char* error_code_string(atpd_error_code_t code) {
    switch (code) {
        case ATPD_ERR_NONE: return "NONE";
        case ATPD_ERR_CONFIG_LOAD: return "CONFIG_LOAD";
        case ATPD_ERR_CONFIG_RELOAD: return "CONFIG_RELOAD";
        case ATPD_ERR_SERVICE_START: return "SERVICE_START";
        case ATPD_ERR_SERVICE_STOP: return "SERVICE_STOP";
        case ATPD_ERR_NETLINK_INIT: return "NETLINK_INIT";
        case ATPD_ERR_APP_FILTER: return "APP_FILTER";
        case ATPD_ERR_GEOIP_UPDATE: return "GEOIP_UPDATE";
        case ATPD_ERR_IPC: return "IPC";
        case ATPD_ERR_TIMEOUT: return "TIMEOUT";
        case ATPD_ERR_MEMORY: return "MEMORY";
        case ATPD_ERR_IO: return "IO";
        case ATPD_ERR_PERMISSION: return "PERMISSION";
        case ATPD_ERR_UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

void atpd_error_init(void) {
    pthread_mutex_lock(&g_error_ring.mutex);
    memset(&g_error_ring.entries, 0, sizeof(g_error_ring.entries));
    g_error_ring.count = 0;
    g_error_ring.head = 0;
    g_error_ring.tail = 0;
    g_error_ring.total_count = 0;
    pthread_mutex_unlock(&g_error_ring.mutex);
}

void atpd_error_push(atpd_error_code_t code, const char *msg, const char *file, int line, const char *func) {
    if (code == ATPD_ERR_NONE || !msg) return;

    pthread_mutex_lock(&g_error_ring.mutex);

    int idx = g_error_ring.tail;
    atpd_error_entry_t *entry = &g_error_ring.entries[idx];

    entry->code = code;
    strncpy(entry->msg, msg, ATPD_ERROR_MSG_LEN - 1);
    entry->msg[ATPD_ERROR_MSG_LEN - 1] = '\0';
    strncpy(entry->file, file, sizeof(entry->file) - 1);
    entry->file[sizeof(entry->file) - 1] = '\0';
    entry->line = line;
    strncpy(entry->func, func, sizeof(entry->func) - 1);
    entry->func[sizeof(entry->func) - 1] = '\0';
    entry->timestamp = time(NULL);

    g_error_ring.tail = (g_error_ring.tail + 1) % ATPD_ERROR_MAX;
    if (g_error_ring.count < ATPD_ERROR_MAX) {
        g_error_ring.count++;
    } else {
        g_error_ring.head = (g_error_ring.head + 1) % ATPD_ERROR_MAX;
    }
    g_error_ring.total_count++;

    LOG_ERROR("[%s] %s (%s:%d %s)", error_code_string(code), msg, file, line, func);

    pthread_mutex_unlock(&g_error_ring.mutex);
}

void atpd_error_clear(void) {
    pthread_mutex_lock(&g_error_ring.mutex);
    memset(&g_error_ring.entries, 0, sizeof(g_error_ring.entries));
    g_error_ring.count = 0;
    g_error_ring.head = 0;
    g_error_ring.tail = 0;
    pthread_mutex_unlock(&g_error_ring.mutex);
}

int atpd_error_count(void) {
    pthread_mutex_lock(&g_error_ring.mutex);
    int count = g_error_ring.count;
    pthread_mutex_unlock(&g_error_ring.mutex);
    return count;
}

const atpd_error_entry_t* atpd_error_get(int index) {
    if (index < 0 || index >= g_error_ring.count) return NULL;
    pthread_mutex_lock(&g_error_ring.mutex);
    int idx = (g_error_ring.head + index) % ATPD_ERROR_MAX;
    const atpd_error_entry_t *entry = &g_error_ring.entries[idx];
    pthread_mutex_unlock(&g_error_ring.mutex);
    return entry;
}

const atpd_error_entry_t* atpd_error_get_last(void) {
    if (g_error_ring.count == 0) return NULL;
    pthread_mutex_lock(&g_error_ring.mutex);
    int idx = (g_error_ring.tail - 1 + ATPD_ERROR_MAX) % ATPD_ERROR_MAX;
    const atpd_error_entry_t *entry = &g_error_ring.entries[idx];
    pthread_mutex_unlock(&g_error_ring.mutex);
    return entry;
}

void atpd_error_print_all(void) {
    pthread_mutex_lock(&g_error_ring.mutex);
    if (g_error_ring.count == 0) {
        LOG_INFO("No errors recorded");
        pthread_mutex_unlock(&g_error_ring.mutex);
        return;
    }

    LOG_INFO("=== Error Log (%d entries, total: %llu) ===",
             g_error_ring.count, (unsigned long long)g_error_ring.total_count);

    for (int i = 0; i < g_error_ring.count; i++) {
        int idx = (g_error_ring.head + i) % ATPD_ERROR_MAX;
        atpd_error_entry_t *entry = &g_error_ring.entries[idx];
        LOG_INFO("[%s] %s", error_code_string(entry->code), entry->msg);
    }
    pthread_mutex_unlock(&g_error_ring.mutex);
}

uint64_t atpd_error_total(void) {
    pthread_mutex_lock(&g_error_ring.mutex);
    uint64_t total = g_error_ring.total_count;
    pthread_mutex_unlock(&g_error_ring.mutex);
    return total;
}
