/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Application filter header
 */

#ifndef ATP_APP_FILTER_H
#define ATP_APP_FILTER_H

#include "atp.h"
#include <stdint.h>

typedef struct {
    int uid;
    char package_name[256];
    int user_id;
} app_info_t;

/* External references for main.c startup summary */
extern int g_current_uids_count;
extern int *g_current_uids;

int app_filter_init(atp_config_t *cfg);
int app_filter_setup(atp_config_t *cfg);
int app_filter_cleanup(atp_config_t *cfg);
int app_filter_reload(atp_config_t *cfg);
int app_filter_get_uid_by_package(const char *package_name, int user_id);
int app_filter_resolve_packages(const char *packages_list, int **uids, int *count);
void app_filter_free_uids(int *uids);

/* Connection-level control (IPv4) */
int app_filter_should_proxy_v4(uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port);
int app_filter_get_connection_uid_v4(uint32_t src_ip, uint16_t src_port,
                                      uint32_t dst_ip, uint16_t dst_port);

/* Connection-level control (IPv6) */
int app_filter_should_proxy_v6(const uint8_t *src_ip, uint16_t src_port,
                                const uint8_t *dst_ip, uint16_t dst_port);
int app_filter_get_connection_uid_v6(const uint8_t *src_ip, uint16_t src_port,
                                      const uint8_t *dst_ip, uint16_t dst_port);

/* Generic wrapper (family-agnostic) */
int app_filter_should_proxy(int family, int protocol,
                             void *src_ip, uint16_t src_port,
                             void *dst_ip, uint16_t dst_port);

#endif /* ATP_APP_FILTER_H */
