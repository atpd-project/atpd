/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Clash API client header
 */

#ifndef ATP_API_H
#define ATP_API_H

#include "atp.h"

/* API retry and rate limit configuration */
#define API_RETRY_COUNT 3
#define API_RETRY_DELAY_MS 500
#define API_MIN_INTERVAL_MS 100

/* External global configuration (defined in main.c) */
extern atp_config_t g_config;

typedef struct {
    char base_url[128];
    char secret[128];
    int retry_count;
    int retry_delay_ms;
    time_t last_call_time;
    int min_interval_ms;
    int last_http_code;
    char last_error[256];
    int timeout_sec;
} api_ctx_t;

typedef enum {
    API_MODE_RULE = 0,
    API_MODE_GLOBAL = 1,
    API_MODE_DIRECT = 2,
    API_MODE_GOOGLE_VPN = 3
} api_mode_t;

/* Global API context (defined in main.c) */
extern api_ctx_t g_api_ctx;

/* Initialize API context */
int api_init(api_ctx_t *ctx, atp_config_t *cfg);

/* API operations */
int api_set_mode(api_ctx_t *ctx, const char *mode);
int api_set_mode_by_enum(api_ctx_t *ctx, api_mode_t mode);
int api_get_config(api_ctx_t *ctx, char *output, size_t size);
int api_get_rules(api_ctx_t *ctx, char *output, size_t size);
int api_get_proxies(api_ctx_t *ctx, char *output, size_t size);
int api_get_health(api_ctx_t *ctx, char *output, size_t size);
int api_get_mode(api_ctx_t *ctx, char *mode, size_t size);

/* Rate limiting */
int api_check_rate_limit(api_ctx_t *ctx);
void api_reset_rate_limit(api_ctx_t *ctx);

/* Mode conversion utilities */
const char *api_mode_to_string(api_mode_t mode);
api_mode_t api_string_to_mode(const char *str);

/* Low-level HTTP operations */
int api_patch_json(api_ctx_t *ctx, const char *path, const char *json_body);
int api_get_json(api_ctx_t *ctx, const char *path, char *output, size_t size);

/* Utility functions */
int api_wait_for_config_loaded(api_ctx_t *ctx, const char *expected_mode, int timeout_sec);
int api_check_health(api_ctx_t *ctx);

#endif /* ATP_API_H */
