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

const char *atpd_error_code_string(atpd_error_code_t code) {
    switch (code) {
        case ATPD_ERR_NONE: return "NONE";
        case ATPD_ERR_CONFIG_LOAD: return "CONFIG_LOAD";
        case ATPD_ERR_CONFIG_RELOAD: return "CONFIG_RELOAD";
        case ATPD_ERR_SERVICE_START: return "SERVICE_START";
        case ATPD_ERR_SERVICE_STOP: return "SERVICE_STOP";
        case ATPD_ERR_NETLINK_INIT: return "NETLINK_INIT";
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
    atpd_error_entry_t snapshot;
    if (code == ATPD_ERR_NONE || !msg) return;

    pthread_mutex_lock(&g_error_ring.mutex);

    int idx = g_error_ring.tail;
    atpd_error_entry_t *entry = &g_error_ring.entries[idx];

    entry->code = code;
    strncpy(entry->msg, msg, ATPD_ERROR_MSG_LEN - 1);
    entry->msg[ATPD_ERROR_MSG_LEN - 1] = '\0';
    strncpy(entry->file, file ? file : "", sizeof(entry->file) - 1);
    entry->file[sizeof(entry->file) - 1] = '\0';
    entry->line = line;
    strncpy(entry->func, func ? func : "", sizeof(entry->func) - 1);
    entry->func[sizeof(entry->func) - 1] = '\0';
    entry->timestamp = time(NULL);

    g_error_ring.tail = (g_error_ring.tail + 1) % ATPD_ERROR_MAX;
    if (g_error_ring.count < ATPD_ERROR_MAX) {
        g_error_ring.count++;
    } else {
        g_error_ring.head = (g_error_ring.head + 1) % ATPD_ERROR_MAX;
    }
    g_error_ring.total_count++;
    snapshot = *entry;
    pthread_mutex_unlock(&g_error_ring.mutex);

    LOG_ERROR("[%s] %s (%s:%d %s)", atpd_error_code_string(snapshot.code),
              snapshot.msg, snapshot.file, snapshot.line, snapshot.func);
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

int atpd_error_get(int index, atpd_error_entry_t *out) {
    if (!out || index < 0) return -1;
    pthread_mutex_lock(&g_error_ring.mutex);
    if (index >= g_error_ring.count) {
        pthread_mutex_unlock(&g_error_ring.mutex);
        return -1;
    }
    int idx = (g_error_ring.head + index) % ATPD_ERROR_MAX;
    *out = g_error_ring.entries[idx];
    pthread_mutex_unlock(&g_error_ring.mutex);
    return 0;
}

int atpd_error_get_last(atpd_error_entry_t *out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_error_ring.mutex);
    if (g_error_ring.count == 0) {
        pthread_mutex_unlock(&g_error_ring.mutex);
        return -1;
    }
    int idx = (g_error_ring.tail - 1 + ATPD_ERROR_MAX) % ATPD_ERROR_MAX;
    *out = g_error_ring.entries[idx];
    pthread_mutex_unlock(&g_error_ring.mutex);
    return 0;
}

void atpd_error_print_all(void) {
    atpd_error_entry_t entries[ATPD_ERROR_MAX];
    int count;
    uint64_t total;
    pthread_mutex_lock(&g_error_ring.mutex);
    count = g_error_ring.count;
    total = g_error_ring.total_count;
    for (int i = 0; i < count; i++) {
        int idx = (g_error_ring.head + i) % ATPD_ERROR_MAX;
        entries[i] = g_error_ring.entries[idx];
    }
    pthread_mutex_unlock(&g_error_ring.mutex);

    if (count == 0) {
        LOG_INFO("No errors recorded");
        return;
    }
    LOG_INFO("=== Error Log (%d entries, total: %llu) ===",
             count, (unsigned long long)total);
    for (int i = 0; i < count; i++) {
        LOG_INFO("[%s] %s", atpd_error_code_string(entries[i].code), entries[i].msg);
    }
}

uint64_t atpd_error_total(void) {
    pthread_mutex_lock(&g_error_ring.mutex);
    uint64_t total = g_error_ring.total_count;
    pthread_mutex_unlock(&g_error_ring.mutex);
    return total;
}
