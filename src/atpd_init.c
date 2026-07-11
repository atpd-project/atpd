/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Initialization phases
 */

#include "config.h"
#include "atpd_init.h"
#include "atpd_global.h"
#include "logger.h"
#include "config.h"
#include "utils.h"
#include "boxbpf.h"
#include "netlink.h"
#include "app_filter.h"
#include "service.h"
#include "api.h"
#include "fcm_monitor.h"
#include "perf_mode.h"
#include "tproxy.h"
#include "cleanup.h"
#include "atpd_context.h"
#include "cli.h"

#include <stdlib.h>

static init_phase_config_t init_phases[] = {
    {INIT_PHASE_CONFIG, "config", atpd_init_phase_config, 1, 0},
    {INIT_PHASE_LOGGER, "logger", atpd_init_phase_logger, 1, 0},
    {INIT_PHASE_EBPF, "ebpf", atpd_init_phase_ebpf, 0, 1},
    {INIT_PHASE_NETLINK, "netlink", atpd_init_phase_netlink, 1, 0},
    {INIT_PHASE_FILTER, "filter", atpd_init_phase_filter, 0, 1},
    {INIT_PHASE_SERVICE, "service", atpd_init_phase_service, 1, 0},
    {INIT_PHASE_API, "api", atpd_init_phase_api, 0, 1},
    {INIT_PHASE_READY, "ready", atpd_init_phase_ready, 1, 0},
};

int atpd_init_phase_config(atpd_init_context_t *ctx) {
    LOG_INFO("Loading configuration...");
    
    config_set_defaults(ctx->config);
    ctx->config->foreground = ctx->opts->foreground;
    ctx->config->verbose = ctx->opts->verbose;
    
    const char *config_path = ctx->opts->config_file;
    if (!config_path || !config_path[0]) {
        config_path = ATP_DEFAULT_DIR "/" ATP_CONF_FILE;
    }
    
    if (config_load(config_path, ctx->config) != ATP_OK) {
        LOG_ERROR("Failed to load config: %s", config_path);
        return -1;
    }
    
    atp_register_cleanup(ctx->config);
    
    return 0;
}

int atpd_init_phase_logger(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing logger...");
    
    logger_init();
    log_set_level(ctx->opts->log_level);
    if (ctx->opts->no_color) {
        log_set_color(0);
    }
    
    return 0;
}

int atpd_init_phase_ebpf(atpd_init_context_t *ctx) {
    if (!ctx->config->bypass_cn_ip || !ctx->config->ebpf_enabled) {
        LOG_DEBUG("eBPF disabled, skipping");
        ctx->config->ebpf_ready = 0;
        atpd_ebpf_state_transition(EBPF_STATE_DISABLED);
        return 0;
    }
    
    if (ctx->config->cnip_mode != 1) {
        LOG_DEBUG("CNIP_MODE is not ebpf, skipping");
        ctx->config->ebpf_ready = 0;
        atpd_ebpf_state_transition(EBPF_STATE_DISABLED);
        return 0;
    }
    
    LOG_INFO("Initializing eBPF CNIP...");
    atpd_ebpf_state_transition(EBPF_STATE_LOADING);
    
    int ret = boxbpf_init_from_config(ctx->config);
    if (ret == ATP_OK) {
        ctx->config->ebpf_ready = 1;
        ctx->ctx->ebpf_enabled = true;
        ctx->ctx->ebpf_probed = true;
        strncpy(ctx->ctx->ebpf_pin_dir, ctx->config->ebpf_pin_dir,
                sizeof(ctx->ctx->ebpf_pin_dir) - 1);
        atpd_ebpf_state_transition(EBPF_STATE_READY);
        LOG_INFO("eBPF CNIP ready (pin: %s)", ctx->config->ebpf_pin_dir);
        return 0;
    } else {
        ctx->config->ebpf_ready = 0;
        ctx->ctx->ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_FAILED);
        LOG_WARN("eBPF CNIP init failed, using ipset fallback");
        return 0;
    }
}

int atpd_init_phase_netlink(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing netlink...");
    
    if (netlink_init(NULL, ctx->config) < 0) {
        LOG_ERROR("Failed to initialize netlink");
        return -1;
    }
    
    netlink_set_reactor(ctx->reactor);
    
    return 0;
}

int atpd_init_phase_filter(atpd_init_context_t *ctx) {
    if (!ctx->config->app_proxy_enable) {
        LOG_DEBUG("App filter disabled, skipping");
        return 0;
    }
    
    LOG_INFO("Initializing app filter...");
    app_filter_init(ctx->config);
    app_filter_setup(ctx->config);
    
    fcm_monitor_init(ctx->config);
    
    return 0;
}

int atpd_init_phase_service(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing service...");
    
    ctx->service = malloc(sizeof(service_ctx_t));
    if (!ctx->service) {
        LOG_ERROR("Failed to allocate service context");
        return -1;
    }
    
    if (service_init(ctx->service, ctx->config) < 0) {
        LOG_ERROR("Failed to initialize service");
        free(ctx->service);
        ctx->service = NULL;
        return -1;
    }
    
    return 0;
}

int atpd_init_phase_api(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing API...");
    
    api_init(ctx->api, ctx->config);
    api_start_with_reactor(ctx->api, ctx->reactor);
    
    return 0;
}

int atpd_init_phase_ready(atpd_init_context_t *ctx) {
    (void)ctx;
    LOG_INFO("All components initialized successfully");
    return 0;
}

int atpd_init_run(atpd_init_context_t *ctx) {
    int failed_phase = INIT_PHASE_MAX;
    
    for (int i = 0; i < INIT_PHASE_MAX; i++) {
        init_phase_config_t *phase = &init_phases[i];
        
        if (phase->handler(ctx) != 0) {
            if (phase->required) {
                LOG_ERROR("Required phase '%s' failed", phase->name);
                failed_phase = i;
                break;
            } else {
                LOG_WARN("Optional phase '%s' failed, continuing", phase->name);
            }
        }
    }
    
    if (failed_phase != INIT_PHASE_MAX) {
        atpd_init_rollback(ctx, failed_phase);
        return -1;
    }
    
    return 0;
}

int atpd_init_rollback(atpd_init_context_t *ctx, init_phase_t phase) {
    LOG_WARN("Rolling back from phase %d", phase);
    
    for (int i = phase; i >= 0; i--) {
        switch (init_phases[i].phase) {
            case INIT_PHASE_SERVICE:
                if (ctx->service) {
                    service_stop_async(ctx->service, NULL, NULL);
                    free(ctx->service);
                    ctx->service = NULL;
                }
                break;
            case INIT_PHASE_EBPF:
                if (ctx->config->ebpf_ready) {
                    boxbpf_clear();
                    ctx->config->ebpf_ready = 0;
                }
                break;
            case INIT_PHASE_NETLINK:
                netlink_cleanup();
                break;
            default:
                break;
        }
    }
    
    return 0;
}
