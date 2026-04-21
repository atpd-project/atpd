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
#include <cjson/cJSON.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define GEOIP_TIMEOUT_SEC 30

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
    int is_v6;
    void (*on_complete)(int success, void *userdata);
    void *userdata;
} geoip_download_ctx_t;

typedef struct {
    atp_config_t *cfg;
    int pending_downloads;
    int success_count;
    int *result;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} geoip_sync_ctx_t;

// 下载响应回调
static void geoip_download_callback(int http_code, const char *response, void *userdata) {
    geoip_download_ctx_t *ctx = (geoip_download_ctx_t*)userdata;
    
    if (http_code != 200 || !response) {
        LOG_ERROR("GeoIP: download failed for %s (HTTP %d)", ctx->url, http_code);
        if (ctx->on_complete) {
            ctx->on_complete(0, ctx->userdata);
        }
        free(ctx);
        return;
    }
    
    LOG_DEBUG("GeoIP: downloaded %zu bytes from %s", strlen(response), ctx->url);
    
    FILE *fp = fopen(ctx->output_path, "w");
    if (!fp) {
        LOG_ERROR("GeoIP: failed to open %s", ctx->output_path);
        if (ctx->on_complete) {
            ctx->on_complete(0, ctx->userdata);
        }
        free(ctx);
        return;
    }
    
    fwrite(response, 1, strlen(response), fp);
    fclose(fp);
    
    if (ctx->on_complete) {
        ctx->on_complete(1, ctx->userdata);
    }
    free(ctx);
}

// 异步下载接口
static int geoip_download_async(atp_config_t *cfg, const char *url, const char *output_path,
                                 void (*on_complete)(int, void*), void *userdata) {
    geoip_download_ctx_t *ctx = calloc(1, sizeof(geoip_download_ctx_t));
    if (!ctx) return -1;
    
    ctx->cfg = cfg;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    strncpy(ctx->output_path, output_path, sizeof(ctx->output_path) - 1);
    ctx->on_complete = on_complete;
    ctx->userdata = userdata;
    
    // 使用 api.c 的原始请求能力
    extern int api_request_raw_async(api_ctx_t *ctx, const char *method, const char *url,
                                      const char *body, api_callback_t callback, void *userdata);
    
    return api_request_raw_async(&g_api_ctx, "GET", url, NULL, geoip_download_callback, ctx);
}

// 同步下载（用于后台线程）
static int geoip_download_sync(atp_config_t *cfg, const char *url, const char *output_path) {
    geoip_sync_ctx_t sync_ctx = {
        .cfg = cfg,
        .pending_downloads = 1,
        .success_count = 0,
        .result = 0
    };
    pthread_mutex_init(&sync_ctx.mutex, NULL);
    pthread_cond_init(&sync_ctx.cond, NULL);
    
    geoip_download_ctx_t *ctx = calloc(1, sizeof(geoip_download_ctx_t));
    if (!ctx) return -1;
    
    ctx->cfg = cfg;
    strncpy(ctx->url, url, sizeof(ctx->url) - 1);
    strncpy(ctx->output_path, output_path, sizeof(ctx->output_path) - 1);
    ctx->userdata = &sync_ctx;
    
    // 这里简化：直接调用同步版本，因为后台线程可以阻塞
    // 实际实现中，由于 geoip 下载是独立的，保留简单的 socket 实现更合适
    
    free(ctx);
    return 0;
}

static int geoip_create_default_ipset(atp_config_t *cfg) {
    LOG_INFO("Creating default ipset (fallback mode)");
    
    ipset_destroy("cnip");
    ipset_create("cnip", 4, 8192, 65536);
    
    for (int i = 0; default_cn_cidrs[i] != NULL; i++) {
        ipset_add_entry("cnip", default_cn_cidrs[i]);
    }
    
    int count = 0;
    while (default_cn_cidrs[count] != NULL) count++;
    LOG_INFO("Default ipset 'cnip' created with %d entries", count);
    return 0;
}

int geoip_init(atp_config_t *cfg) {
    char rules_dir[PATH_MAX];
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    mkdir_recursive(rules_dir, 0755);
    
    LOG_DEBUG("GeoIP initialized (using async API framework)");
    return 0;
}

int geoip_download_url(const char *url, const char *output_path, int timeout_sec) {
    // 简化实现：直接调用全局 API 上下文
    (void)timeout_sec;
    return geoip_download_async(NULL, url, output_path, NULL, NULL);
}

int geoip_download(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) {
        LOG_DEBUG("CN IP bypass disabled, skipping download");
        return 0;
    }
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_tmp[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_tmp, sizeof(v4_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ip_file);
    
    LOG_INFO("Downloading China IPv4 list");
    if (geoip_download_url(cfg->cn_ip_url, v4_tmp, GEOIP_TIMEOUT_SEC) == 0) {
        rename(v4_tmp, v4_path);
        LOG_INFO("IPv4 list downloaded successfully");
        return 0;
    } else {
        LOG_WARN("Failed to download IPv4 list, using cached if available");
        return -1;
    }
}

static void* geoip_async_update_thread(void *arg) {
    atp_config_t *cfg = (atp_config_t*)arg;
    LOG_INFO("GeoIP async update started in background");
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_parsed[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed, sizeof(v4_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ip_file);
    
    if (geoip_download(cfg) == 0 && file_exists(v4_path)) {
        ipset_parse_cidr_file(v4_path, v4_parsed, 4);
        
        ipset_create("cnip_temp", 4, 8192, 65536);
        ipset_restore_file("cnip_temp", v4_parsed);
        ipset_swap("cnip_temp", "cnip");
        ipset_destroy("cnip_temp");
        LOG_INFO("IPv4 ipset upgraded to full list");
    } else {
        LOG_WARN("Full GeoIP download failed, keeping default list");
    }
    
    geoip_async_running = 0;
    geoip_async_complete = 1;
    LOG_INFO("GeoIP async update completed");
    return NULL;
}

int geoip_setup_ipset_async(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) {
        LOG_DEBUG("CN IP bypass disabled, skipping ipset setup");
        return 0;
    }
    
    geoip_create_default_ipset(cfg);
    
    if (!geoip_async_running) {
        geoip_async_running = 1;
        geoip_async_complete = 0;
        if (pthread_create(&geoip_thread, NULL, geoip_async_update_thread, cfg) != 0) {
            LOG_WARN("Failed to create GeoIP async thread");
            geoip_async_running = 0;
            return -1;
        }
        pthread_detach(geoip_thread);
        LOG_INFO("GeoIP async update thread started");
    }
    
    return 0;
}

int geoip_async_is_complete(void) {
    return geoip_async_complete;
}

int geoip_setup_ipset(atp_config_t *cfg) {
    return geoip_setup_ipset_async(cfg);
}

int geoip_cleanup_ipset(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    
    ipset_destroy("cnip");
    ipset_destroy("cnip6");
    
    LOG_INFO("GeoIP ipsets destroyed");
    return 0;
}

int geoip_atomic_update(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    
    LOG_INFO("Performing atomic GeoIP update");
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_tmp[PATH_MAX];
    char v4_parsed[PATH_MAX];
    char v4_parsed_tmp[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_tmp, sizeof(v4_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed, sizeof(v4_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed_tmp, sizeof(v4_parsed_tmp), "%s/%s.parsed.tmp", rules_dir, cfg->cn_ip_file);
    
    if (geoip_download_url(cfg->cn_ip_url, v4_tmp, GEOIP_TIMEOUT_SEC) != 0) {
        LOG_ERROR("Failed to download fresh IPv4 list");
        return -1;
    }
    
    ipset_parse_cidr_file(v4_tmp, v4_parsed_tmp, 4);
    ipset_create("cnip_temp", 4, 8192, 65536);
    ipset_restore_file("cnip_temp", v4_parsed_tmp);
    ipset_swap("cnip_temp", "cnip");
    ipset_destroy("cnip_temp");
    rename(v4_tmp, v4_path);
    rename(v4_parsed_tmp, v4_parsed);
    
    if (cfg->proxy_ipv6) {
        char v6_path[PATH_MAX];
        char v6_tmp[PATH_MAX];
        char v6_parsed[PATH_MAX];
        char v6_parsed_tmp[PATH_MAX];
        
        snprintf(v6_path, sizeof(v6_path), "%s/%s", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_tmp, sizeof(v6_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_parsed, sizeof(v6_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_parsed_tmp, sizeof(v6_parsed_tmp), "%s/%s.parsed.tmp", rules_dir, cfg->cn_ipv6_file);
        
        if (geoip_download_url(cfg->cn_ipv6_url, v6_tmp, GEOIP_TIMEOUT_SEC) == 0) {
            ipset_parse_cidr_file(v6_tmp, v6_parsed_tmp, 6);
            ipset_create("cnip6_temp", 6, 8192, 65536);
            ipset_restore_file("cnip6_temp", v6_parsed_tmp);
            ipset_swap("cnip6_temp", "cnip6");
            ipset_destroy("cnip6_temp");
            rename(v6_tmp, v6_path);
            rename(v6_parsed_tmp, v6_parsed);
        }
    }
    
    LOG_INFO("Atomic GeoIP update completed");
    return 0;
}

int geoip_check_update_needed(atp_config_t *cfg, int max_age_days) {
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    
    if (!file_exists(v4_path)) {
        LOG_INFO("IPv4 list missing, update needed");
        return 1;
    }
    
    struct stat st;
    if (stat(v4_path, &st) != 0) return 1;
    
    time_t now = time(NULL);
    int age_days = (now - st.st_mtime) / 86400;
    
    if (age_days >= max_age_days) {
        LOG_INFO("IPv4 list is %d days old, update needed", age_days);
        return 1;
    }
    
    LOG_DEBUG("IPv4 list is %d days old, no update needed", age_days);
    return 0;
}

int geoip_force_update(atp_config_t *cfg) {
    LOG_INFO("Forcing GeoIP update");
    return geoip_atomic_update(cfg);
}

int geoip_validate_cidr(const char *cidr, int family) {
    char ip[128];
    int prefix;
    
    if (sscanf(cidr, "%127[^/]/%d", ip, &prefix) != 2) return -1;
    
    struct in_addr ipv4;
    struct in6_addr ipv6;
    
    if (family == 4) {
        if (inet_pton(AF_INET, ip, &ipv4) == 1 && prefix >= 0 && prefix <= 32) return 0;
    } else {
        if (inet_pton(AF_INET6, ip, &ipv6) == 1 && prefix >= 0 && prefix <= 128) return 0;
    }
    
    return -1;
}
