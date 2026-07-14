/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration reload
 */

#include "atpd_global.h"
#include "logger.h"
#include "config.h"
#include "app_filter.h"

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
