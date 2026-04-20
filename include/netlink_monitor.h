/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Netlink monitor header
 */

#ifndef ATP_NETLINK_MONITOR_H
#define ATP_NETLINK_MONITOR_H

#include "atp.h"

int netlink_monitor_init(atp_config_t *cfg);
void netlink_monitor_cleanup(void);
int netlink_monitor_get_fd(void);
int netlink_monitor_is_running(void);
int netlink_monitor_poll(int timeout_ms);
void netlink_monitor_handle(void);

#endif
