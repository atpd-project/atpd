#ifndef ATP_FCM_MONITOR_H
#define ATP_FCM_MONITOR_H

#include "atp_config.h"
#include <stdint.h>
#include <time.h>
#include <stdatomic.h>

#define FCM_DEFAULT_PORT 5228

typedef void (*fcm_callback_t)(const char *dst_ip, uint16_t dst_port, void *userdata);

typedef struct {
    uint64_t dns_refresh_success;
    uint64_t dns_refresh_failed;
    uint64_t tracked_table_full;
    uint64_t cache_full;
    uint64_t cache_entries;
    uint64_t tracked_entries;
    uint64_t resolved_domain_total;
    uint64_t failed_domain_total;
    uint64_t dns_duration_ms;
    time_t   last_detection;
} fcm_monitor_stats_t;

int fcm_monitor_init(atp_config_t *cfg);
int fcm_monitor_start(fcm_callback_t callback, void *userdata);
void fcm_monitor_stop(void);
int fcm_monitor_is_running(void);
void fcm_monitor_poll(void);
time_t fcm_monitor_get_last_detection(void);
void fcm_monitor_refresh_cache(void);
int fcm_monitor_get_fd(void);
void fcm_monitor_handle(void);
void fcm_monitor_cleanup(void);
int fcm_monitor_get_stats(fcm_monitor_stats_t *stats);

#endif
