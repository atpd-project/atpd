/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration reload
 */

#include "atpd_global.h"
#include "logger.h"
#include "config.h"
#include "boxbpf.h"
#include "app_filter.h"
#include "service.h"
#include "api.h"

#include <string.h>
#include <time.h>

int atpd_reload_config(void) {
    LOG_INFO("Reloading configuration...");
    
    int ret = config_reload(&g_config);
    if (ret != ATP_OK) {
        LOG_ERROR("Configuration reload failed");
        return -1;
    }
    
    LOG_INFO("Configuration reloaded successfully");
    return 0;
}

int atpd_apply_config(void) {
    if (g_config.app_proxy_enable) {
        app_filter_reload(&g_config);
    }
    
    if (g_config.ebpf_enabled && g_config.bypass_cn_ip) {
        ebpf_reload(&g_config);
    }
    
    return 0;
}
