#include <pthread.h>
#ifndef ATPD_ERROR_H
#define ATPD_ERROR_H

#include <stdint.h>
#include <time.h>

#define ATPD_ERROR_MAX 128
#define ATPD_ERROR_MSG_LEN 256

typedef enum {
    ATPD_ERR_NONE = 0,
    ATPD_ERR_CONFIG_LOAD,
    ATPD_ERR_CONFIG_RELOAD,
    ATPD_ERR_EBPF_INIT,
    ATPD_ERR_EBPF_RELOAD,
    ATPD_ERR_SERVICE_START,
    ATPD_ERR_SERVICE_STOP,
    ATPD_ERR_NETLINK_INIT,
    ATPD_ERR_APP_FILTER,
    ATPD_ERR_GEOIP_UPDATE,
    ATPD_ERR_IPC,
    ATPD_ERR_TIMEOUT,
    ATPD_ERR_MEMORY,
    ATPD_ERR_IO,
    ATPD_ERR_PERMISSION,
    ATPD_ERR_UNKNOWN
} atpd_error_code_t;

typedef struct {
    atpd_error_code_t code;
    char msg[ATPD_ERROR_MSG_LEN];
    char file[64];
    int line;
    char func[64];
    uint64_t timestamp;
} atpd_error_entry_t;

typedef struct {
    atpd_error_entry_t entries[ATPD_ERROR_MAX];
    int count;
    int head;
    int tail;
    uint64_t total_count;
    pthread_mutex_t mutex;
} atpd_error_ring_t;

void atpd_error_init(void);
void atpd_error_push(atpd_error_code_t code, const char *msg, const char *file, int line, const char *func);
void atpd_error_clear(void);
int atpd_error_count(void);
const atpd_error_entry_t* atpd_error_get(int index);
const atpd_error_entry_t* atpd_error_get_last(void);
void atpd_error_print_all(void);
uint64_t atpd_error_total(void);

#define ATPD_ERROR(code, msg) \
    atpd_error_push(code, msg, __FILE__, __LINE__, __FUNCTION__)

#endif
