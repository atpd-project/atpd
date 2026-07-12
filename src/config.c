#include "config.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include "atpd_context.h"
#include "boxbpf.h"
#include "app_filter.h"
#include <pwd.h>
#include <grp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)

static config_snapshot_t g_snapshot = {
    .has_backup = 0,
    .backup_path = "",
    .version = 0,
    .load_time = 0
};

void config_set_defaults(atp_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(atp_config_t));
    pthread_mutex_init(&cfg->mutex, NULL);

    /* Core */
    cfg->core.foreground = 0;
    cfg->core.verbose = 0;
    cfg->core.no_color = 0;
    cfg->core.ui_emoji_enabled = 1;
    cfg->core.performance_mode = 1;
    cfg->core.dry_run = 0;
    cfg->core.skip_check_feature = 0;
    cfg->core.force_mark_bypass = 0;
    cfg->core.log_timestamp = 1;
    cfg->core.proxy_tcp = 1;
    cfg->core.proxy_udp = 1;
    cfg->core.block_quic = 1;
    cfg->core.restart_delay = DEFAULT_RESTART_DELAY;
    snprintf(cfg->core.data_dir, sizeof(cfg->core.data_dir), "%s", ATP_DEFAULT_DIR);
    snprintf(cfg->core.core_user, sizeof(cfg->core.core_user), "root");
    snprintf(cfg->core.core_group, sizeof(cfg->core.core_group), "net_admin");
    cfg->core.routing_mark[0] = '\0';

    /* Network */
    cfg->network.use_tproxy = 1;
    cfg->network.proxy_mode = MODE_ENHANCE;
    cfg->network.tcp_port = 7891;
    cfg->network.udp_port = DEFAULT_UDP_PORT;
    cfg->network.redirect_tcp_port = DEFAULT_REDIRECT_TCP_PORT;
    cfg->network.mark_value = DEFAULT_MARK;
    cfg->network.mark_value6 = DEFAULT_MARK6;
    cfg->network.table_id = 150;
    cfg->network.proxy_ipv6 = 1;
    cfg->network.dns_hijack = DNS_HIJACK_TPROXY;
    cfg->network.dns_port = DEFAULT_DNS_PORT;

    /* Interface */
    snprintf(cfg->interface.mobile_iface, sizeof(cfg->interface.mobile_iface), "rmnet_data+");
    snprintf(cfg->interface.wifi_iface, sizeof(cfg->interface.wifi_iface), "wlan0");
    snprintf(cfg->interface.hotspot_iface, sizeof(cfg->interface.hotspot_iface), "wlan2");
    snprintf(cfg->interface.usb_iface, sizeof(cfg->interface.usb_iface), "rndis+");
    snprintf(cfg->interface.hotspot_subnet_ipv4, sizeof(cfg->interface.hotspot_subnet_ipv4), "192.168.43.0/24");
    snprintf(cfg->interface.hotspot_subnet_ipv6, sizeof(cfg->interface.hotspot_subnet_ipv6), "fe80::/10");
    cfg->interface.current_vpn_iface[0] = '\0';
    snprintf(cfg->interface.other_bypass, sizeof(cfg->interface.other_bypass), "tun0 ipsec+");
    cfg->interface.other_proxy[0] = '\0';
    cfg->interface.proxy_mobile = 1;
    cfg->interface.proxy_wifi = 1;
    cfg->interface.proxy_hotspot = 1;
    cfg->interface.proxy_usb = 1;

    /* Filter */
    cfg->filter.app_proxy_enable = 1;
    cfg->filter.mac_filter_enable = 0;
    cfg->filter.bypass_cn_ip = 1;
    cfg->filter.cnip_mode = 1;
    snprintf(cfg->filter.app_proxy_mode, sizeof(cfg->filter.app_proxy_mode), "blacklist");
    snprintf(cfg->filter.mac_proxy_mode, sizeof(cfg->filter.mac_proxy_mode), "blacklist");
    snprintf(cfg->filter.user_clash_mode, sizeof(cfg->filter.user_clash_mode), "Rule");
    cfg->filter.clash_secret[0] = '\0';
    snprintf(cfg->filter.proxy_apps_list, sizeof(cfg->filter.proxy_apps_list), "");
    snprintf(cfg->filter.bypass_apps_list, sizeof(cfg->filter.bypass_apps_list),
             "0:com.android.systemui 0:com.miui.home");
    cfg->filter.proxy_macs_list[0] = '\0';
    cfg->filter.bypass_macs_list[0] = '\0';
    cfg->filter.cnip_force_proxy_apps[0] = '\0';
    snprintf(cfg->filter.cn_ip_url, sizeof(cfg->filter.cn_ip_url),
             "https://raw.githubusercontent.com/Hackl0us/GeoIP2-CN/release/CN-ip-cidr.txt");
    snprintf(cfg->filter.cn_ipv6_url, sizeof(cfg->filter.cn_ipv6_url),
             "https://ispip.clang.cn/all_cn_ipv6.txt");
    snprintf(cfg->filter.cn_ip_file, sizeof(cfg->filter.cn_ip_file), "cn.zone");
    snprintf(cfg->filter.cn_ipv6_file, sizeof(cfg->filter.cn_ipv6_file), "cn_ipv6.zone");

    /* IP Lists */
    snprintf(cfg->iplist.bypass_ipv4_list, sizeof(cfg->iplist.bypass_ipv4_list),
             "0.0.0.0/8 10.0.0.0/8 100.0.0.0/8 127.0.0.0/8 169.254.0.0/16 "
             "172.16.0.0/12 192.168.0.0/16 224.0.0.0/4 240.0.0.0/4 255.255.255.255/32");
    snprintf(cfg->iplist.bypass_ipv6_list, sizeof(cfg->iplist.bypass_ipv6_list),
             "::/128 ::1/128 ::ffff:0:0/96 100::/64 64:ff9b::/96 2001::/32 "
             "2001:10::/28 2001:20::/28 2001:db8::/32 2002::/16 fe80::/10 ff00::/8");
    cfg->iplist.proxy_ipv4_list[0] = '\0';
    cfg->iplist.proxy_ipv6_list[0] = '\0';

    /* eBPF */
    cfg->ebpf.enabled = 1;
    cfg->ebpf.ready = 0;
    cfg->ebpf.load_retry = 3;
    cfg->ebpf.load_delay = 2;
    snprintf(cfg->ebpf.bin_path, sizeof(cfg->ebpf.bin_path), "%s/bin/boxbpf", ATP_DEFAULT_DIR);
    snprintf(cfg->ebpf.pin_dir, sizeof(cfg->ebpf.pin_dir), "/sys/fs/bpf/box");
    snprintf(cfg->ebpf.state_dir, sizeof(cfg->ebpf.state_dir), "%s/ebpf", ATP_DEFAULT_DIR);
    snprintf(cfg->ebpf.config_path, sizeof(cfg->ebpf.config_path), "%s/ebpf/config.json", ATP_DEFAULT_DIR);

    /* Service */
    cfg->service.start_timeout_sec = SERVICE_DEFAULT_START_TIMEOUT_SEC;
    cfg->service.stop_timeout_sec = SERVICE_DEFAULT_STOP_TIMEOUT_SEC;
    cfg->service.grace_period_sec = SERVICE_DEFAULT_GRACE_PERIOD_SEC;
    cfg->service.max_failures = SERVICE_DEFAULT_MAX_FAILURES;
    cfg->service.circuit_threshold = SERVICE_DEFAULT_CIRCUIT_THRESHOLD;
    cfg->service.circuit_cooldown_sec = SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC;
    cfg->service.health_check_interval_ms = SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS;
    cfg->service.args[0] = '\0';
    cfg->service.env[0] = '\0';

    /* API */
    cfg->api.port = DEFAULT_API_PORT;
    snprintf(cfg->api.host, sizeof(cfg->api.host), "%s", DEFAULT_API_HOST);
}

static void parse_key_value(const char *k, const char *v, atp_config_t *cfg) {
    if (strcmp(k, "PROXY_TCP_PORT") == 0) cfg->network.tcp_port = atoi(v);
    else if (strcmp(k, "PROXY_UDP_PORT") == 0) cfg->network.udp_port = atoi(v);
    else if (strcmp(k, "REDIRECT_TCP_PORT") == 0) cfg->network.redirect_tcp_port = atoi(v);
    else if (strcmp(k, "PROXY_MODE") == 0) cfg->network.proxy_mode = atoi(v);
    else if (strcmp(k, "PERFORMANCE_MODE") == 0) cfg->core.performance_mode = atoi(v);
    else if (strcmp(k, "PROXY_TCP") == 0) cfg->core.proxy_tcp = atoi(v);
    else if (strcmp(k, "PROXY_UDP") == 0) cfg->core.proxy_udp = atoi(v);
    else if (strcmp(k, "PROXY_IPV6") == 0) cfg->network.proxy_ipv6 = atoi(v);
    else if (strcmp(k, "SKIP_CHECK_FEATURE") == 0) cfg->core.skip_check_feature = atoi(v);
    else if (strcmp(k, "DNS_HIJACK_ENABLE") == 0) cfg->network.dns_hijack = atoi(v);
    else if (strcmp(k, "DNS_PORT") == 0) cfg->network.dns_port = atoi(v);
    else if (strcmp(k, "MARK_VALUE") == 0) cfg->network.mark_value = atoi(v);
    else if (strcmp(k, "MARK_VALUE6") == 0) cfg->network.mark_value6 = atoi(v);
    else if (strcmp(k, "TABLE_ID") == 0) cfg->network.table_id = atoi(v);
    else if (strcmp(k, "ROUTING_MARK") == 0) snprintf(cfg->core.routing_mark, sizeof(cfg->core.routing_mark), "%s", v);
    else if (strcmp(k, "FORCE_MARK_BYPASS") == 0) cfg->core.force_mark_bypass = atoi(v);
    else if (strcmp(k, "MOBILE_INTERFACE") == 0) snprintf(cfg->interface.mobile_iface, sizeof(cfg->interface.mobile_iface), "%s", v);
    else if (strcmp(k, "WIFI_INTERFACE") == 0) snprintf(cfg->interface.wifi_iface, sizeof(cfg->interface.wifi_iface), "%s", v);
    else if (strcmp(k, "HOTSPOT_INTERFACE") == 0) snprintf(cfg->interface.hotspot_iface, sizeof(cfg->interface.hotspot_iface), "%s", v);
    else if (strcmp(k, "USB_INTERFACE") == 0) snprintf(cfg->interface.usb_iface, sizeof(cfg->interface.usb_iface), "%s", v);
    else if (strcmp(k, "OTHER_BYPASS_INTERFACES") == 0) snprintf(cfg->interface.other_bypass, sizeof(cfg->interface.other_bypass), "%s", v);
    else if (strcmp(k, "OTHER_PROXY_INTERFACES") == 0) snprintf(cfg->interface.other_proxy, sizeof(cfg->interface.other_proxy), "%s", v);
    else if (strcmp(k, "PROXY_MOBILE") == 0) cfg->interface.proxy_mobile = atoi(v);
    else if (strcmp(k, "PROXY_WIFI") == 0) cfg->interface.proxy_wifi = atoi(v);
    else if (strcmp(k, "PROXY_HOTSPOT") == 0) cfg->interface.proxy_hotspot = atoi(v);
    else if (strcmp(k, "PROXY_USB") == 0) cfg->interface.proxy_usb = atoi(v);
    else if (strcmp(k, "HOTSPOT_SUBNET_IPV4") == 0) snprintf(cfg->interface.hotspot_subnet_ipv4, sizeof(cfg->interface.hotspot_subnet_ipv4), "%s", v);
    else if (strcmp(k, "HOTSPOT_SUBNET_IPV6") == 0) snprintf(cfg->interface.hotspot_subnet_ipv6, sizeof(cfg->interface.hotspot_subnet_ipv6), "%s", v);
    else if (strcmp(k, "PROXY_IPv4_LIST") == 0) snprintf(cfg->iplist.proxy_ipv4_list, sizeof(cfg->iplist.proxy_ipv4_list), "%s", v);
    else if (strcmp(k, "PROXY_IPv6_LIST") == 0) snprintf(cfg->iplist.proxy_ipv6_list, sizeof(cfg->iplist.proxy_ipv6_list), "%s", v);
    else if (strcmp(k, "BYPASS_IPv4_LIST") == 0) snprintf(cfg->iplist.bypass_ipv4_list, sizeof(cfg->iplist.bypass_ipv4_list), "%s", v);
    else if (strcmp(k, "BYPASS_IPv6_LIST") == 0) snprintf(cfg->iplist.bypass_ipv6_list, sizeof(cfg->iplist.bypass_ipv6_list), "%s", v);
    else if (strcmp(k, "BYPASS_CN_IP") == 0) cfg->filter.bypass_cn_ip = atoi(v);
    else if (strcmp(k, "CN_IP_FILE") == 0) snprintf(cfg->filter.cn_ip_file, sizeof(cfg->filter.cn_ip_file), "%s", v);
    else if (strcmp(k, "CN_IPV6_FILE") == 0) snprintf(cfg->filter.cn_ipv6_file, sizeof(cfg->filter.cn_ipv6_file), "%s", v);
    else if (strcmp(k, "CN_IP_URL") == 0) snprintf(cfg->filter.cn_ip_url, sizeof(cfg->filter.cn_ip_url), "%s", v);
    else if (strcmp(k, "CN_IPV6_URL") == 0) snprintf(cfg->filter.cn_ipv6_url, sizeof(cfg->filter.cn_ipv6_url), "%s", v);
    else if (strcmp(k, "APP_PROXY_ENABLE") == 0) cfg->filter.app_proxy_enable = atoi(v);
    else if (strcmp(k, "PROXY_APPS_LIST") == 0) snprintf(cfg->filter.proxy_apps_list, sizeof(cfg->filter.proxy_apps_list), "%s", v);
    else if (strcmp(k, "BYPASS_APPS_LIST") == 0) snprintf(cfg->filter.bypass_apps_list, sizeof(cfg->filter.bypass_apps_list), "%s", v);
    else if (strcmp(k, "APP_PROXY_MODE") == 0) snprintf(cfg->filter.app_proxy_mode, sizeof(cfg->filter.app_proxy_mode), "%s", v);
    else if (strcmp(k, "MAC_FILTER_ENABLE") == 0) cfg->filter.mac_filter_enable = atoi(v);
    else if (strcmp(k, "PROXY_MACS_LIST") == 0) snprintf(cfg->filter.proxy_macs_list, sizeof(cfg->filter.proxy_macs_list), "%s", v);
    else if (strcmp(k, "BYPASS_MACS_LIST") == 0) snprintf(cfg->filter.bypass_macs_list, sizeof(cfg->filter.bypass_macs_list), "%s", v);
    else if (strcmp(k, "MAC_PROXY_MODE") == 0) snprintf(cfg->filter.mac_proxy_mode, sizeof(cfg->filter.mac_proxy_mode), "%s", v);
    else if (strcmp(k, "BLOCK_QUIC") == 0) cfg->core.block_quic = atoi(v);
    else if (strcmp(k, "LOG_TIMESTAMP") == 0) cfg->core.log_timestamp = atoi(v);
    else if (strcmp(k, "USER_CLASH_MODE") == 0) snprintf(cfg->filter.user_clash_mode, sizeof(cfg->filter.user_clash_mode), "%s", v);
    else if (strcmp(k, "RESTART_DELAY") == 0) cfg->core.restart_delay = atoi(v);
    else if (strcmp(k, "CLASH_SECRET") == 0) snprintf(cfg->filter.clash_secret, sizeof(cfg->filter.clash_secret), "%s", v);
    else if (strcmp(k, "API_PORT") == 0) cfg->api.port = atoi(v);
    else if (strcmp(k, "API_HOST") == 0) snprintf(cfg->api.host, sizeof(cfg->api.host), "%s", v);
    else if (strcmp(k, "UI_EMOJI_ENABLED") == 0) cfg->core.ui_emoji_enabled = atoi(v);
    else if (strcmp(k, "ENABLE_EBPF") == 0) cfg->ebpf.enabled = atoi(v);
    else if (strcmp(k, "CNIP_MODE") == 0) {
        if (strcmp(v, "ebpf") == 0) cfg->filter.cnip_mode = 1;
        else if (strcmp(v, "ipset") == 0) cfg->filter.cnip_mode = 0;
        else cfg->filter.cnip_mode = atoi(v);
    }
    else if (strcmp(k, "EBPF_BIN") == 0) {
        snprintf(cfg->ebpf.bin_path, sizeof(cfg->ebpf.bin_path), "%s", v);
    }
    else if (strcmp(k, "EBPF_PIN_DIR") == 0) {
        snprintf(cfg->ebpf.pin_dir, sizeof(cfg->ebpf.pin_dir), "%s", v);
    }
    else if (strcmp(k, "EBPF_STATE_DIR") == 0) {
        snprintf(cfg->ebpf.state_dir, sizeof(cfg->ebpf.state_dir), "%s", v);
    }
    else if (strcmp(k, "EBPF_LOAD_RETRY") == 0) {
        cfg->ebpf.load_retry = atoi(v);
    }
    else if (strcmp(k, "EBPF_LOAD_DELAY") == 0) {
        cfg->ebpf.load_delay = atoi(v);
    }
    else if (strcmp(k, "CNIP_FORCE_PROXY_APPS") == 0) {
        snprintf(cfg->filter.cnip_force_proxy_apps, sizeof(cfg->filter.cnip_force_proxy_apps), "%s", v);
    }
    else if (strcmp(k, "CORE_USER_GROUP") == 0) {
        char val[256]; snprintf(val, sizeof(val), "%s", v); char *colon = strchr(val, ':');
        if (colon) {
            *colon = '\0';
            snprintf(cfg->core.core_user, sizeof(cfg->core.core_user), "%.63s", val);
            snprintf(cfg->core.core_group, sizeof(cfg->core.core_group), "%.63s", colon + 1);
        }
    }
    else if (strcmp(k, "SERVICE_START_TIMEOUT") == 0) cfg->service.start_timeout_sec = atoi(v);
    else if (strcmp(k, "SERVICE_STOP_TIMEOUT") == 0) cfg->service.stop_timeout_sec = atoi(v);
    else if (strcmp(k, "SERVICE_GRACE_PERIOD") == 0) cfg->service.grace_period_sec = atoi(v);
    else if (strcmp(k, "SERVICE_MAX_FAILURES") == 0) cfg->service.max_failures = atoi(v);
    else if (strcmp(k, "SERVICE_CIRCUIT_THRESHOLD") == 0) cfg->service.circuit_threshold = atoi(v);
    else if (strcmp(k, "SERVICE_CIRCUIT_COOLDOWN") == 0) cfg->service.circuit_cooldown_sec = atoi(v);
    else if (strcmp(k, "SERVICE_HEALTH_CHECK_INTERVAL") == 0) cfg->service.health_check_interval_ms = atoi(v);
    else if (strcmp(k, "SERVICE_ARGS") == 0) snprintf(cfg->service.args, sizeof(cfg->service.args), "%s", v);
    else if (strcmp(k, "SERVICE_ENV") == 0) snprintf(cfg->service.env, sizeof(cfg->service.env), "%s", v);
}

int config_load(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->mutex);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        pthread_mutex_unlock(&cfg->mutex);
        return ATP_ERR_NOENT;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim(line); if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = '\0'; char *k = line, *v = eq + 1; trim(k); trim(v);
        if ((v[0] == '"' || v[0] == '\'') && strlen(v) >= 2) {
            size_t vlen = strlen(v); if (v[vlen-1] == v[0]) { v[vlen-1] = '\0'; memmove(v, v+1, vlen-1); }
        }
        parse_key_value(k, v, cfg);
    }
    fclose(fp);
    pthread_mutex_unlock(&cfg->mutex);
    LOG_INFO("Configuration loaded: %s", path);
    return ATP_OK;
}

int config_set_mode(atp_config_t *cfg, const char *mode) {
    pthread_mutex_lock(&cfg->mutex);
    snprintf(cfg->filter.user_clash_mode, sizeof(cfg->filter.user_clash_mode), "%s", mode);
    char data_dir[SAFE_PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s", cfg->core.data_dir);
    pthread_mutex_unlock(&cfg->mutex);

    char cp[SAFE_PATH_MAX], tp[SAFE_PATH_MAX];
    if (snprintf(cp, sizeof(cp), "%s/%s", data_dir, ATP_CONF_FILE) >= (int)sizeof(cp)) return ATP_ERR_INVAL;
    if (!file_exists(cp)) return ATP_OK;
    if (snprintf(tp, sizeof(tp), "%s.tmp", cp) >= (int)sizeof(tp)) return ATP_ERR_INVAL;
    FILE *fin = fopen(cp, "r"), *fout = fopen(tp, "w");
    if (!fin || !fout) { if (fin) fclose(fin); if (fout) fclose(fout); return ATP_ERR_IO; }
    char line[1024]; int found = 0;
    while (fgets(line, sizeof(line), fin)) {
        if (strncmp(line, "USER_CLASH_MODE=", 16) == 0) { fprintf(fout, "USER_CLASH_MODE=\"%s\"\n", mode); found = 1; }
        else fputs(line, fout);
    }
    if (!found) fprintf(fout, "USER_CLASH_MODE=\"%s\"\n", mode);
    fclose(fin); fclose(fout);
    if (rename(tp, cp) != 0) return ATP_ERR_IO;
    return ATP_OK;
}

static void ebpf_reload(atp_config_t *cfg) {
    if (!cfg->filter.bypass_cn_ip) {
        LOG_DEBUG("CNIP bypass disabled, skipping eBPF reload");
        return;
    }
    if (!cfg->ebpf.enabled) {
        LOG_DEBUG("eBPF disabled, skipping eBPF reload");
        return;
    }
    if (cfg->filter.cnip_mode != 1) {
        LOG_DEBUG("CNIP_MODE is not ebpf, skipping eBPF reload");
        return;
    }

    int ret = boxbpf_reload_from_config(cfg);
    if (ret == ATP_OK) {
        cfg->ebpf.ready = 1;
        atpd_ebpf_state_transition(EBPF_STATE_READY);
        LOG_INFO("eBPF CNIP reloaded successfully");
    } else {
        cfg->ebpf.ready = 0;
        atpd_ebpf_state_transition(EBPF_STATE_FAILED);
        LOG_ERROR("eBPF CNIP reload failed");
    }
}

int config_reload(atp_config_t *cfg) {
    char cp[SAFE_PATH_MAX];
    if (snprintf(cp, sizeof(cp), "%s/%s", cfg->core.data_dir, ATP_CONF_FILE) >= (int)sizeof(cp)) {
        return ATP_ERR_INVAL;
    }
    if (!file_exists(cp)) {
        return ATP_ERR_NOENT;
    }

    int ret = config_load(cp, cfg);
    if (ret == ATP_OK) {
        ebpf_reload(cfg);
        if (cfg->filter.app_proxy_enable) {
            if (app_filter_reload(cfg) == ATP_OK) {
                LOG_INFO("App filter reloaded successfully");
            } else {
                LOG_ERROR("App filter reload failed");
            }
        }
        LOG_INFO("Configuration reloaded successfully");
    }
    return ret;
}

int config_reload_atomic(atp_config_t *cfg) {
    char cp[SAFE_PATH_MAX];
    if (snprintf(cp, sizeof(cp), "%s/%s", cfg->core.data_dir, ATP_CONF_FILE) >= (int)sizeof(cp)) {
        return ATP_ERR_INVAL;
    }
    if (!file_exists(cp)) {
        return ATP_ERR_NOENT;
    }

    atp_config_t new_config;
    memcpy(&new_config, cfg, sizeof(atp_config_t));
    pthread_mutex_init(&new_config.mutex, NULL);

    int ret = config_load(cp, &new_config);
    if (ret != ATP_OK) {
        LOG_ERROR("Atomic reload: failed to load new config");
        return ret;
    }

    char backup_path[SAFE_PATH_MAX];
    snprintf(backup_path, sizeof(backup_path), "%s/runtime_atp.conf.backup", cfg->core.data_dir);
    config_save_runtime(backup_path, cfg);

    memcpy(cfg, &new_config, sizeof(atp_config_t));

    g_snapshot.has_backup = 1;
    snprintf(g_snapshot.backup_path, sizeof(g_snapshot.backup_path), "%s", backup_path);
    g_snapshot.version++;
    g_snapshot.load_time = time(NULL);

    ebpf_reload(cfg);
    if (cfg->filter.app_proxy_enable) {
        if (app_filter_reload(cfg) != ATP_OK) {
            LOG_ERROR("App filter reload failed after atomic reload");
        }
    }

    LOG_INFO("Atomic reload completed (version: %llu)", (unsigned long long)g_snapshot.version);
    return ATP_OK;
}

int config_rollback(atp_config_t *cfg) {
    if (!g_snapshot.has_backup) {
        LOG_ERROR("No backup available for rollback");
        return ATP_ERR_NOENT;
    }

    if (!file_exists(g_snapshot.backup_path)) {
        LOG_ERROR("Backup file not found: %s", g_snapshot.backup_path);
        g_snapshot.has_backup = 0;
        return ATP_ERR_NOENT;
    }

    atp_config_t backup_config;
    memcpy(&backup_config, cfg, sizeof(atp_config_t));
    pthread_mutex_init(&backup_config.mutex, NULL);

    int ret = config_load(g_snapshot.backup_path, &backup_config);
    if (ret != ATP_OK) {
        LOG_ERROR("Rollback: failed to load backup config");
        return ret;
    }

    memcpy(cfg, &backup_config, sizeof(atp_config_t));
    g_snapshot.has_backup = 0;

    ebpf_reload(cfg);
    if (cfg->filter.app_proxy_enable) {
        app_filter_reload(cfg);
    }

    LOG_INFO("Configuration rolled back to previous version");
    return ATP_OK;
}

const config_snapshot_t* config_get_snapshot(void) {
    return &g_snapshot;
}

int config_save_runtime(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->mutex);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        pthread_mutex_unlock(&cfg->mutex);
        return ATP_ERR_IO;
    }
    fprintf(fp, "# ATP Runtime\nUSE_TPROXY=%d\nTABLE_ID=%d\nMARK_VALUE=%d\nMARK_VALUE6=%d\nEBPF_ENABLED=%d\nCNIP_MODE=%d\nEBPF_READY=%d\nEBPF_PIN_DIR=%s\n",
            cfg->network.use_tproxy, cfg->network.table_id, cfg->network.mark_value, cfg->network.mark_value6,
            cfg->ebpf.enabled, cfg->filter.cnip_mode, cfg->ebpf.ready, cfg->ebpf.pin_dir);
    fclose(fp);
    pthread_mutex_unlock(&cfg->mutex);
    return ATP_OK;
}

int validate_interface_name(const char *n) {
    if (!n || !*n || strlen(n) > 15) return ATP_ERR_INVAL;
    for (const char *p = n; *p; p++) if (!isalnum(*p) && !strchr("_+-.", *p)) return ATP_ERR_INVAL;
    return ATP_OK;
}

int validate_port(int p) {
    if (p > 0 && p <= 65535) return ATP_OK;
    return ATP_ERR_INVAL;
}

int validate_mark(int m) {
    if (m >= 1 && m <= 2147483647) return ATP_OK;
    return ATP_ERR_INVAL;
}
