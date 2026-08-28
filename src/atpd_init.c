/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Initialization phases - Pure eBPF Architecture
 */

#include "config.h"
#include "atpd_init.h"
#include "atpd_global.h"
#include "logger.h"
#include "utils.h"
#include "ebpf.h"
#include "netlink.h"
#include "service.h"
#include "api.h"
#include "cleanup.h"
#include "atpd_context.h"
#include "cli.h"

#include <stdlib.h>
#include <unistd.h>

static init_phase_config_t init_phases[] = {
    {INIT_PHASE_CONFIG, "config", atpd_init_phase_config, 1, 0},
    {INIT_PHASE_LOGGER, "logger", atpd_init_phase_logger, 1, 0},
    {INIT_PHASE_EBPF, "ebpf", atpd_init_phase_ebpf, 1, 0},
    {INIT_PHASE_NETLINK, "netlink", atpd_init_phase_netlink, 1, 0},
    {INIT_PHASE_SERVICE, "service", atpd_init_phase_service, 1, 0},
    {INIT_PHASE_API, "api", atpd_init_phase_api, 0, 1},
    {INIT_PHASE_READY, "ready", atpd_init_phase_ready, 1, 0},
};

int atpd_init_phase_config(atpd_init_context_t *ctx) {
    LOG_INFO("Loading configuration...");
    
    char auto_cfg_path[PATH_MAX];
    const char *config_path = ctx->opts->config_file;
    if (!config_path || !config_path[0]) {
        if (snprintf(auto_cfg_path, sizeof(auto_cfg_path), "%s/%s",
                     ctx->config->core.data_dir, ATP_CONF_FILE) >= (int)sizeof(auto_cfg_path)) {
            LOG_ERROR("Configuration path is too long");
            return -1;
        }
        config_path = auto_cfg_path;
    }
    
    if (access(config_path, R_OK) == 0) {
        if (config_load(config_path, ctx->config) != ATP_OK) {
            LOG_ERROR("Invalid configuration: %s", config_path);
            return -1;
        }
    } else if (ctx->opts->config_file[0]) {
        LOG_ERROR("Cannot read configuration: %s", config_path);
        return -1;
    }

    /* Command-line options override both defaults and file configuration. */
    ctx->config->core.foreground = ctx->opts->foreground;
    ctx->config->core.verbose = ctx->opts->verbose;
    
    atp_register_cleanup(ctx->config);
    
    return 0;
}

int atpd_init_phase_logger(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing logger...");
    
    logger_init();
    if (ctx->config->core.log_file[0]) {
        char log_path[PATH_MAX];
        if (ctx->config->core.log_file[0] == '/') {
            snprintf(log_path, sizeof(log_path), "%s", ctx->config->core.log_file);
        } else {
            if (snprintf(log_path, sizeof(log_path), "%s/%s",
                         ctx->config->core.data_dir[0] ? ctx->config->core.data_dir : ".",
                         ctx->config->core.log_file) >= (int)sizeof(log_path)) {
                LOG_ERROR("Log path is too long");
                return -1;
            }
        }
        char log_dir[PATH_MAX];
        snprintf(log_dir, sizeof(log_dir), "%s", log_path);
        char *slash = strrchr(log_dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir_recursive(log_dir, 0755);
        }
        log_set_file(log_path);
    }
    log_set_level(ctx->opts->log_level);
    if (ctx->opts->no_color) {
        log_set_color(0);
    }
    
    return 0;
}

int atpd_init_phase_ebpf(atpd_init_context_t *ctx) {
    LOG_INFO("Pure eBPF Engine: probing kernel eBPF capabilities...");
    atpd_ebpf_state_transition(EBPF_STATE_LOADING);
    
    int ret = ebpf_probe();
    if (ret == ATP_OK) {
        ctx->config->ebpf.ready = 1;
        ctx->ctx->ebpf_enabled = true;
        ctx->ctx->ebpf_probed = true;
        atpd_ebpf_state_transition(EBPF_STATE_READY);
        LOG_INFO("Pure eBPF Engine: active (Zero-iptables / cgroup kernel interception)");
        return 0;
    } else {
        ctx->config->ebpf.ready = 0;
        ctx->ctx->ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_FAILED);
        LOG_WARN("Pure eBPF Engine: kernel eBPF probe returned warnings or unsupported features");
        return 0;
    }
}

int atpd_init_phase_netlink(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing Netlink & Multi-VPN tunnel listener...");
    
    if (netlink_init(NULL, ctx->config) < 0) {
        LOG_ERROR("Failed to initialize netlink");
        return -1;
    }
    
    if (ctx->reactor) {
        netlink_set_reactor(ctx->reactor);
        netlink_xfrm_init(ctx->reactor);
    }
    
    return 0;
}

int atpd_init_phase_service(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing sing-box core service supervisor...");
    
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
    LOG_INFO("Initializing sing-box Native API client...");
    
    api_init(ctx->api, ctx->config);
    atpd_set_vpn_mode_callback(api_vpn_mode_callback, ctx->api);
    if (ctx->reactor) {
        api_start_with_reactor(ctx->api, ctx->reactor);
    }
    
    return 0;
}

int atpd_init_phase_ready(atpd_init_context_t *ctx) {
    (void)ctx;
    LOG_INFO("Pure eBPF Environment ready - all components initialized");
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
            case INIT_PHASE_API:
                if (ctx->api) {
                    api_cleanup(ctx->api);
                }
                break;
            case INIT_PHASE_SERVICE:
                if (ctx->service) {
                    service_stop_async(ctx->service, NULL, NULL);
                    free(ctx->service);
                    ctx->service = NULL;
                }
                break;
            case INIT_PHASE_EBPF:
                ctx->config->ebpf.ready = 0;
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
