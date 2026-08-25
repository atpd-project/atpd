#include "atpd_global.h"
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

void api_vpn_mode_callback(vpn_state_t state, const char *iface, void *userdata) {
    api_ctx_t *ctx = userdata;
    if (!ctx) return;

    if (state == VPN_STATE_PREDICTING || state == VPN_STATE_TEARDOWN) {
        /* The debounced READY/IDLE transition is the stable sync point. */
        return;
    }

    if (state == VPN_STATE_READY && (!iface || strncmp(iface, "ipsec", 5) != 0)) {
        return;
    }

    singbox_clash_mode_status_t status;
    if (singbox_api_get_clash_mode_status(&ctx->native_ctx, &status) != 0) {
        LOG_WARN("Native API: Clash mode service unavailable during VPN sync");
        return;
    }

    if (state == VPN_STATE_IDLE) {
        if (!ctx->default_mode[0] || strcmp(status.current_mode, ctx->default_mode) == 0) {
            return;
        }
        if (api_set_mode_async(ctx, ctx->default_mode, NULL, NULL) != 0) {
            LOG_WARN("Native API: failed to restore Clash mode to %s", ctx->default_mode);
            return;
        }
        LOG_INFO("Native API: VPN state IDLE restored Clash mode %s", ctx->default_mode);
        return;
    }

    if (!ctx->default_mode[0] && strcmp(status.current_mode, "Google VPN") != 0) {
        snprintf(ctx->default_mode, sizeof(ctx->default_mode), "%s", status.current_mode);
    }

    int google_vpn_available = 0;
    for (size_t i = 0; i < status.mode_count; ++i) {
        if (strcmp(status.modes[i], "Google VPN") == 0) {
            google_vpn_available = 1;
            break;
        }
    }
    if (!google_vpn_available) {
        LOG_WARN("Native API: Google VPN Clash mode is not present in the calculated mode list");
        return;
    }

    if (strcmp(status.current_mode, "Google VPN") == 0) return;
    if (api_set_mode_async(ctx, "Google VPN", NULL, NULL) != 0) {
        LOG_WARN("Native API: failed to switch Clash mode to Google VPN");
        return;
    }
    LOG_INFO("Native API: VPN state READY selected Clash mode Google VPN (restore=%s)",
             ctx->default_mode[0] ? ctx->default_mode : "unavailable");
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
