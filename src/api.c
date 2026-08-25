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

int api_get_version_sync(api_ctx_t *ctx, char *version, size_t size) {
    if (!ctx || !version || size == 0) return -1;
    return singbox_api_get_version(&ctx->native_ctx, version, size);
}

int api_get_goroutines_count(api_ctx_t *ctx) {
    if (!ctx) return -1;
    int count = -1;
    if (singbox_api_get_goroutines(&ctx->native_ctx, &count) == 0 && count >= 0) {
        return count;
    }
    return -1;
}
