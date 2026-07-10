/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * GeoIP download - Synchronous mode
 */

#include "geoip.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include "ipset.h"
#include "api.h"
#include "boxbpf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define GEOIP_TIMEOUT_SEC 30
#define SAFE_PATH_MAX (PATH_MAX + 256)
#define GEOIP_RESPONSE_MAX (4 * 1024 * 1024)

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

static int geoip_create_default_ipset(atp_config_t *cfg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would create default ipset cnip with %d entries",
                  (int)(sizeof(default_cn_cidrs) / sizeof(default_cn_cidrs[0]) - 1));
        return 0;
    }
    LOG_INFO("Creating default ipset (fallback mode)");
    ipset_destroy("cnip");
    ipset_create("cnip", 4, 8192, 65536);
    for (int i = 0; default_cn_cidrs[i] != NULL; i++) ipset_add_entry("cnip", default_cn_cidrs[i]);
    return 0;
}

int geoip_init(atp_config_t *cfg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would initialize GeoIP rules directory");
        return 0;
    }
    char rules_dir[SAFE_PATH_MAX];
    if (snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir) < (int)sizeof(rules_dir)) {
        mkdir_recursive(rules_dir, 0755);
    }
    LOG_DEBUG("GeoIP initialized");
    return 0;
}

int geoip_download_url(const char *url, const char *output_path, int timeout_sec) {
    (void)timeout_sec;

    char *response = malloc(GEOIP_RESPONSE_MAX);
    if (!response) {
        LOG_ERROR("GeoIP: malloc failed for download buffer");
        return -1;
    }

    if (api_get_sync(url, response, GEOIP_RESPONSE_MAX) != 0) {
        LOG_ERROR("GeoIP: download failed for %s", url);
        free(response);
        return -1;
    }

    FILE *fp = fopen(output_path, "w");
    if (!fp) {
        LOG_ERROR("GeoIP: failed to open %s", output_path);
        free(response);
        return -1;
    }
    fwrite(response, 1, strlen(response), fp);
    fclose(fp);
    free(response);
    return 0;
}

int geoip_download(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would download GeoIP from %s", cfg->cn_ip_url);
        return 0;
    }
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
    if (cfg->dry_run) {
        geoip_async_running = 0; geoip_async_complete = 1;
        return NULL;
    }
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
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would start async GeoIP update");
        return 0;
    }
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
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would destroy ipset cnip and cnip6");
        return 0;
    }
    ipset_destroy("cnip"); ipset_destroy("cnip6");
    return 0;
}

int geoip_atomic_update(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would atomically update GeoIP ipset from %s", cfg->cn_ip_url);
        return 0;
    }

    char v4_p[SAFE_PATH_MAX], v4_t[SAFE_PATH_MAX], v4_r[SAFE_PATH_MAX], v4_rt[SAFE_PATH_MAX];
    char v6_p[SAFE_PATH_MAX], v6_t[SAFE_PATH_MAX], v6_r[SAFE_PATH_MAX], v6_rt[SAFE_PATH_MAX];
    int ret = 0;

    snprintf(v4_p, sizeof(v4_p), "%s/rules/%.255s", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_t, sizeof(v4_t), "%s/rules/%.255s.tmp", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_r, sizeof(v4_r), "%s/rules/%.255s.parsed", cfg->data_dir, cfg->cn_ip_file);
    snprintf(v4_rt, sizeof(v4_rt), "%s/rules/%.255s.parsed.tmp", cfg->data_dir, cfg->cn_ip_file);

    if (geoip_download_url(cfg->cn_ip_url, v4_t, GEOIP_TIMEOUT_SEC) != 0) {
        LOG_ERROR("GeoIP: failed to download IPv4 list");
        return -1;
    }
    ipset_parse_cidr_file(v4_t, v4_rt, 4);
    ipset_create("cnip_temp", 4, 8192, 65536);
    ipset_restore_file("cnip_temp", v4_rt);
    ipset_swap("cnip_temp", "cnip");
    ipset_destroy("cnip_temp");
    rename(v4_t, v4_p);
    rename(v4_rt, v4_r);

    if (cfg->proxy_ipv6) {
        snprintf(v6_p, sizeof(v6_p), "%s/rules/%.255s", cfg->data_dir, cfg->cn_ipv6_file);
        snprintf(v6_t, sizeof(v6_t), "%s/rules/%.255s.tmp", cfg->data_dir, cfg->cn_ipv6_file);
        snprintf(v6_r, sizeof(v6_r), "%s/rules/%.255s.parsed", cfg->data_dir, cfg->cn_ipv6_file);
        snprintf(v6_rt, sizeof(v6_rt), "%s/rules/%.255s.parsed.tmp", cfg->data_dir, cfg->cn_ipv6_file);

        if (geoip_download_url(cfg->cn_ipv6_url, v6_t, GEOIP_TIMEOUT_SEC) != 0) {
            LOG_WARN("GeoIP: failed to download IPv6 list, skipping");
        } else {
            ipset_parse_cidr_file(v6_t, v6_rt, 6);
            ipset_create("cnip6_temp", 6, 8192, 65536);
            ipset_restore_file("cnip6_temp", v6_rt);
            ipset_swap("cnip6_temp", "cnip6");
            ipset_destroy("cnip6_temp");
            rename(v6_t, v6_p);
            rename(v6_rt, v6_r);
        }
    }

    if (cfg->ebpf_ready && cfg->ebpf_enabled && cfg->cnip_mode == 1) {
        LOG_INFO("GeoIP: updating eBPF CNIP maps...");
        if (write_ebpf_config(cfg) == 0) {
            if (boxbpf_update(cfg->ebpf_config_path) == 0) {
                LOG_INFO("GeoIP: eBPF CNIP maps updated");
            } else {
                LOG_WARN("GeoIP: eBPF CNIP maps update failed");
                ret = -1;
            }
        } else {
            LOG_WARN("GeoIP: failed to regenerate eBPF config");
            ret = -1;
        }
    }

    return ret;
}

int geoip_check_update_needed(atp_config_t *cfg, int max_age_days) {
    if (cfg->dry_run) return 0;
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

int geoip_ipset_create(const char *name, int family, int hashsize, int maxelem) {
    return ipset_create(name, family, hashsize, maxelem);
}

int geoip_ipset_destroy(const char *name) {
    return ipset_destroy(name);
}

int geoip_ipset_swap(const char *from, const char *to) {
    return ipset_swap(from, to);
}

int geoip_ipset_exists(const char *name) {
    return ipset_exists(name);
}

int geoip_ipset_flush(const char *name) {
    return ipset_flush(name);
}

int geoip_ipset_restore_file(const char *name, const char *filename) {
    return ipset_restore_file(name, filename);
}

int geoip_parse_cidr_file(const char *input_path, const char *output_path, int family) {
    return ipset_parse_cidr_file(input_path, output_path, family);
}
