/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * GeoIP download - Adapted to async API framework
 */

#include "geoip.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include "ipset.h"
#include "api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <yyjson.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define GEOIP_TIMEOUT_SEC 30
#define SAFE_PATH_MAX (PATH_MAX + 256)

static pthread_t geoip_thread;
static int geoip_async_running = 0;
static int geoip_async_complete = 0;

static const char *default_cn_cidrs[] = {
    "1.0.0.0/8", "14.0.0.0/8", "27.0.0.0/8", "36.0.0.0/8",
    "39.0.0.0/8", "42.0.0.0/8", "49.0.0.0/8", "58.0.0.0/8",
    "59.0.0.0/8", "60.0.0.0/8", "61.0.0.0/8", "101.0.0.0/8",
    "106.0.0.0/8", "110.0.0.0/8", "111.0.0.0/8", "112.0.0.0/8",
    "113.0.0.0/8", "114.0.0.0/8", "115.0.0.0/8", "116.0.0.0/8",
    "117.0.0.0/8", "118.0.0.0/8", "119.0.0.0/8", "120.0.0.0/8",
    "121.0.0.0/8", "122.0.0.0/8", "123.0.0.0/8", "124.0.0.0/8",
    "125.0.0.0/8", "126.0.0.0/8", "169.254.0.0/16", "172.16.0.0/12",
    "192.168.0.0/16", "223.0.0.0/8", NULL
};

typedef struct {
    atp_config_t *cfg;
    char url[512];
    char output_path[PATH_MAX];
    void (*on_complete)(int success, void *userdata);
    void *userdata;
} geoip_download_ctx_t;

static void geoip_download_callback(int http_code, const char *response, void *userdata) {
    geoip_download_ctx_t *ctx = (geoip_download_ctx_t*)userdata;
    if (http_code != 200 || !response) {
        LOG_ERROR("GeoIP: download failed for %s (HTTP %d)", ctx->url, http_code);
        if (ctx->on_complete) ctx->on_complete(0, ctx->userdata);
        free(ctx); return;
    }
    FILE *fp = fopen(ctx->output_path, "w");
    if (!fp) {
        LOG_ERROR("GeoIP: failed to open %s", ctx->output_path);
        if (ctx->on_complete) ctx->on_complete(0, ctx->userdata);
        free(ctx); return;
    }
    fwrite(response, 1, strlen(response), fp);
    fclose(fp);
    if (ctx->on_complete) ctx->on_complete(1, ctx->userdata);
    free(ctx);
}

static int geoip_download_async(atp_config_t *cfg, const char *url, const char *output_path,
                                 void (*on_complete)(int, void*), void *userdata) {
    geoip_download_ctx_t *ctx = calloc(1, sizeof(geoip_download_ctx_t));
    if (!ctx) return -1;
    ctx->cfg = cfg;
    snprintf(ctx->url, sizeof(ctx->url), "%.511s", url);
    snprintf(ctx->output_path, sizeof(ctx->output_path), "%.4095s", output_path);
    ctx->on_complete = on_complete;
    ctx->userdata = userdata;
    extern int api_request_raw_async(api_ctx_t *ctx, const char *method, const char *url,
                                      const char *body, api_callback_t callback, void *userdata);
    return api_request_raw_async(&g_api_ctx, "GET", url, NULL, geoip_download_callback, ctx);
}

static int geoip_create_default_ipset(atp_config_t *cfg) {
    (void)cfg;
    LOG_INFO("Creating default ipset (fallback mode)");
    ipset_destroy("cnip");
    ipset_create("cnip", 4, 8192, 65536);
    for (int i = 0; default_cn_cidrs[i] != NULL; i++) ipset_add_entry("cnip", default_cn_cidrs[i]);
    return 0;
}

int geoip_init(atp_config_t *cfg) {
    char rules_dir[SAFE_PATH_MAX];
    if (snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir) < (int)sizeof(rules_dir)) {
        mkdir_recursive(rules_dir, 0755);
    }
    LOG_DEBUG("GeoIP initialized");
    return 0;
}

int geoip_download_url(const char *url, const char *output_path, int timeout_sec) {
    (void)timeout_sec;
    return geoip_download_async(NULL, url, output_path, NULL, NULL);
}

int geoip_download(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    char v4_p[SAFE_PATH_MAX], v4_t[SAFE_PATH_MAX];
    snprintf(v4_p, sizeof(v4_p), "%s/rules/%.255s", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_t, sizeof(v4_t), "%s/rules/%.255s.tmp", cfg->data_dir, cfg->cn_ip_file);
    if (geoip_download_url(cfg->cn_ip_url, v4_t, GEOIP_TIMEOUT_SEC) == 0) {
        rename(v4_t, v4_p); return 0;
    }
    return -1;
}

static void* geoip_async_update_thread(void *arg) {
    atp_config_t *cfg = (atp_config_t*)arg;
    char v4_p[SAFE_PATH_MAX], v4_r[SAFE_PATH_MAX];
    snprintf(v4_p, sizeof(v4_p), "%s/rules/%.255s", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_r, sizeof(v4_r), "%s/rules/%.255s.parsed", cfg->data_dir, cfg->cn_ip_file);
    if (geoip_download(cfg) == 0 && file_exists(v4_p)) {
        ipset_parse_cidr_file(v4_p, v4_r, 4);
        ipset_create("cnip_temp", 4, 8192, 65536);
        ipset_restore_file("cnip_temp", v4_r);
        ipset_swap("cnip_temp", "cnip");
        ipset_destroy("cnip_temp");
    }
    geoip_async_running = 0; geoip_async_complete = 1;
    return NULL;
}

int geoip_setup_ipset_async(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    geoip_create_default_ipset(cfg);
    if (!geoip_async_running) {
        geoip_async_running = 1; geoip_async_complete = 0;
        if (pthread_create(&geoip_thread, NULL, geoip_async_update_thread, cfg) == 0)
            pthread_detach(geoip_thread);
        else geoip_async_running = 0;
    }
    return 0;
}

int geoip_async_is_complete(void) { return geoip_async_complete; }
int geoip_setup_ipset(atp_config_t *cfg) { return geoip_setup_ipset_async(cfg); }

int geoip_cleanup_ipset(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    ipset_destroy("cnip"); ipset_destroy("cnip6");
    return 0;
}

int geoip_atomic_update(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    char v4_p[SAFE_PATH_MAX], v4_t[SAFE_PATH_MAX], v4_r[SAFE_PATH_MAX], v4_rt[SAFE_PATH_MAX];
    snprintf(v4_p, sizeof(v4_p), "%s/rules/%.255s", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_t, sizeof(v4_t), "%s/rules/%.255s.tmp", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_r, sizeof(v4_r), "%s/rules/%.255s.parsed", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_rt, sizeof(v4_rt), "%s/rules/%.255s.parsed.tmp", cfg->data_dir, cfg->cn_ip_file);
    if (geoip_download_url(cfg->cn_ip_url, v4_t, GEOIP_TIMEOUT_SEC) != 0) return -1;
    ipset_parse_cidr_file(v4_t, v4_rt, 4);
    ipset_create("cnip_temp", 4, 8192, 65536);
    ipset_restore_file("cnip_temp", v4_rt);
    ipset_swap("cnip_temp", "cnip");
    ipset_destroy("cnip_temp");
    rename(v4_t, v4_p); rename(v4_rt, v4_r);
    return 0;
}

int geoip_check_update_needed(atp_config_t *cfg, int max_age_days) {
    char v4_p[SAFE_PATH_MAX];
    snprintf(v4_p, sizeof(v4_p), "%s/rules/%.255s", cfg->data_dir, cfg->cn_ip_file);
    struct stat st;
    if (stat(v4_p, &st) != 0) return 1;
    return ((int)((time(NULL) - st.st_mtime) / 86400) >= max_age_days);
}

int geoip_force_update(atp_config_t *cfg) { return geoip_atomic_update(cfg); }

int geoip_validate_cidr(const char *cidr, int family) {
    char ip[128]; int prefix;
    if (sscanf(cidr, "%127[^/]/%d", ip, &prefix) != 2) return -1;
    struct in_addr v4; struct in6_addr v6;
    if (family == 4) return (inet_pton(AF_INET, ip, &v4) == 1 && prefix <= 32) ? 0 : -1;
    return (inet_pton(AF_INET6, ip, &v6) == 1 && prefix <= 128) ? 0 : -1;
}
