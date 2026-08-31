/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * sing-box Native API Dispatcher & Controller
 */

#include "api.h"
#include "logger.h"
#include "utils.h"
#include "reactor.h"
#include "singbox_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static reactor_t *g_api_reactor = NULL;

int api_init(api_ctx_t *ctx, atp_config_t *cfg) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(api_ctx_t));
    ctx->config = cfg;

    singbox_api_init(&ctx->native_ctx, cfg);

    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d",
             ctx->native_ctx.host, ctx->native_ctx.port);

    if (ctx->native_ctx.secret[0]) {
        snprintf(ctx->secret, sizeof(ctx->secret), "%s", ctx->native_ctx.secret);
    }

    ctx->timeout_sec = 2;

    LOG_INFO("sing-box Native API dispatcher ready on %s:%d",
             ctx->native_ctx.host, ctx->native_ctx.port);
    return 0;
}

void api_cleanup(api_ctx_t *ctx) {
    if (!ctx) return;
    singbox_api_cleanup(&ctx->native_ctx);
    g_api_reactor = NULL;
}

int api_start_with_reactor(api_ctx_t *ctx, reactor_t *r) {
    if (!ctx || !r) return -1;
    g_api_reactor = r;
    LOG_INFO("sing-box Native API dispatcher bound to reactor");
    return 0;
}

int api_check_health_sync(api_ctx_t *ctx) {
    if (!ctx) return -1;
    return singbox_api_health_check(&ctx->native_ctx);
}

int api_check_health_async(api_ctx_t *ctx, api_callback_t callback, void *userdata) {
    int ret = api_check_health_sync(ctx);
    if (callback) {
        callback(ret == 0 ? 200 : 503, ret == 0 ? "OK" : "Service Unavailable", userdata);
    }
    return ret;
}

int api_get_mode_sync(api_ctx_t *ctx, char *mode, size_t size) {
    if (!ctx || !mode || size == 0) return -1;
    return singbox_api_get_clash_mode(&ctx->native_ctx, mode, size);
}

int api_set_mode_async(api_ctx_t *ctx, const char *mode, api_callback_t callback, void *userdata) {
    if (!ctx || !mode) return -1;
    int ret = singbox_api_set_clash_mode(&ctx->native_ctx, mode);
    if (callback) {
        callback(ret == 0 ? 200 : 500, ret == 0 ? "OK" : "Failed", userdata);
    }
    return ret;
}

static int is_clash_mode_supported(const singbox_clash_mode_status_t *status, const char *mode) {
    if (!status || !mode || !mode[0]) return 0;
    for (size_t i = 0; i < status->mode_count; ++i) {
        if (strcmp(status->modes[i], mode) == 0) return 1;
    }
    return 0;
}

void api_vpn_mode_callback(vpn_state_t state, const char *iface, void *userdata) {
    api_ctx_t *ctx = userdata;
    if (!ctx) return;

    if (!ctx->config || !ctx->config->interface.vpn_auto_mode) {
        return;
    }

    if (state == VPN_STATE_PREDICTING || state == VPN_STATE_TEARDOWN) {
        /* The debounced READY/IDLE transition is the stable sync point. */
        return;
    }

    const char *target_mode = ctx->config->interface.vpn_target_mode[0] ?
                              ctx->config->interface.vpn_target_mode : "Google VPN";
    const char *fallback_mode = ctx->config->interface.vpn_fallback_mode[0] ?
                                ctx->config->interface.vpn_fallback_mode : "Rule";

    singbox_clash_mode_status_t status;
    if (singbox_api_get_clash_mode_status(&ctx->native_ctx, &status) != 0) {
        LOG_WARN("Native API: Clash mode service unavailable during VPN sync");
        return;
    }

    if (state == VPN_STATE_IDLE) {
        if (!ctx->default_mode[0]) {
            return;
        }
        const char *restore_mode = ctx->default_mode;
        if (!is_clash_mode_supported(&status, restore_mode)) {
            if (!is_clash_mode_supported(&status, fallback_mode)) {
                LOG_WARN("Native API: neither saved Clash mode '%s' nor fallback '%s' is available",
                         restore_mode, fallback_mode);
                return;
            }
            restore_mode = fallback_mode;
        }
        if (strcmp(status.current_mode, restore_mode) != 0 &&
            api_set_mode_async(ctx, restore_mode, NULL, NULL) != 0) {
            LOG_WARN("Native API: failed to restore Clash mode to %s", restore_mode);
            return;
        }
        LOG_INFO("Native API: VPN state IDLE restored Clash mode %s", restore_mode);
        ctx->default_mode[0] = '\0';
        return;
    }

    /* state == VPN_STATE_READY */
    if (!ctx->default_mode[0]) {
        snprintf(ctx->default_mode, sizeof(ctx->default_mode), "%s", status.current_mode);
    }

    if (!is_clash_mode_supported(&status, target_mode)) {
        LOG_WARN("Native API: Target Clash mode '%s' is not present in sing-box mode list", target_mode);
        return;
    }

    if (strcmp(status.current_mode, target_mode) == 0) return;
    if (api_set_mode_async(ctx, target_mode, NULL, NULL) != 0) {
        LOG_WARN("Native API: failed to switch Clash mode to %s", target_mode);
        return;
    }
    LOG_INFO("Native API: VPN state READY (%s) selected Clash mode '%s' (restore=%s)",
             (iface && iface[0]) ? iface : "vpn", target_mode,
             ctx->default_mode[0] ? ctx->default_mode : fallback_mode);
}

int api_get_version_sync(api_ctx_t *ctx, char *version, size_t size) {
    if (!ctx || !version || size == 0) return -1;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (singbox_api_get_version(&ctx->native_ctx, version, size) == 0 && version[0]) {
            return 0;
        }
        if (attempt < 19) usleep(100 * 1000);
    }
    LOG_WARN("sing-box GetVersion gRPC-Web query unavailable after startup retries");
    return -1;
}

int api_get_status_sync(api_ctx_t *ctx, singbox_status_t *status) {
    if (!ctx || !status) return -1;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (singbox_api_get_status(&ctx->native_ctx, status) == 0 && status->goroutines > 0) {
            return 0;
        }
        if (attempt < 19) usleep(100 * 1000);
    }
    LOG_WARN("sing-box SubscribeStatus gRPC-Web snapshot unavailable after startup retries");
    return -1;
}

int api_get_goroutines_count(api_ctx_t *ctx) {
    if (!ctx) return -1;
    singbox_status_t status;
    if (api_get_status_sync(ctx, &status) == 0) return status.goroutines;
    return -1;
}
