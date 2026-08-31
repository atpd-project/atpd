#ifndef ATP_STATUS_H
#define ATP_STATUS_H

#include "atp.h"
#include "service.h"
#include "api.h"
#include <stdint.h>

typedef struct {
    uint64_t collected_at_ms;
    pid_t atpd_pid;
    int atpd_uptime_sec;
    int atpd_fd_count;
    int atpd_thread_count;
    long atpd_rss_kb;
    long atpd_hwm_kb;
    pid_t singbox_pid;
    int singbox_uptime_sec;
    int singbox_fd_count;
    int singbox_thread_count;
} status_snapshot_t;

int status_collect_snapshot(const atp_config_t *cfg, service_ctx_t *svc,
                            status_snapshot_t *out);

void status_show(atp_config_t *cfg, service_ctx_t *svc, api_ctx_t *api);
void status_show_config(atp_config_t *cfg);

#endif
