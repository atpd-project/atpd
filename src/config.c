#include "config.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <pwd.h>
#include <grp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)

void config_set_defaults(atp_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(atp_config_t));
    pthread_mutex_init(&cfg->config_mutex, NULL);
    snprintf(cfg->data_dir, sizeof(cfg->data_dir), "%s", ATP_DEFAULT_DIR);
    cfg->tcp_port = DEFAULT_TCP_PORT;
    cfg->udp_port = DEFAULT_UDP_PORT;
    cfg->redirect_tcp_port = DEFAULT_REDIRECT_TCP_PORT;
    cfg->proxy_mode = MODE_AUTO;
    cfg->proxy_tcp = 1; cfg->proxy_udp = 1;
    cfg->dns_hijack = DNS_HIJACK_TPROXY;
    cfg->dns_port = DEFAULT_DNS_PORT;
    cfg->mark_value = DEFAULT_MARK;
    cfg->mark_value6 = DEFAULT_MARK6;
    cfg->table_id = DEFAULT_TABLE_ID;
    cfg->restart_delay = DEFAULT_RESTART_DELAY;
    cfg->api_port = DEFAULT_API_PORT;
    cfg->ui_emoji_enabled = 1;
    snprintf(cfg->core_user, sizeof(cfg->core_user), "root");
    snprintf(cfg->core_group, sizeof(cfg->core_group), "net_admin");
    snprintf(cfg->mobile_iface, sizeof(cfg->mobile_iface), "rmnet_data+");
    snprintf(cfg->wifi_iface, sizeof(cfg->wifi_iface), "wlan0");
    snprintf(cfg->hotspot_iface, sizeof(cfg->hotspot_iface), "wlan2");
    snprintf(cfg->usb_iface, sizeof(cfg->usb_iface), "rndis+");
    cfg->proxy_mobile = 1; cfg->proxy_wifi = 1;
    snprintf(cfg->hotspot_subnet_ipv4, sizeof(cfg->hotspot_subnet_ipv4), "192.168.43.0/24");
    snprintf(cfg->cn_ip_file, sizeof(cfg->cn_ip_file), "cn.zone");
    snprintf(cfg->cn_ipv6_file, sizeof(cfg->cn_ipv6_file), "cn_ipv6.zone");
    snprintf(cfg->app_proxy_mode, sizeof(cfg->app_proxy_mode), "blacklist");
    snprintf(cfg->mac_proxy_mode, sizeof(cfg->mac_proxy_mode), "blacklist");
    snprintf(cfg->user_clash_mode, sizeof(cfg->user_clash_mode), "Rule");
    snprintf(cfg->api_host, sizeof(cfg->api_host), "%s", DEFAULT_API_HOST);
}

static void parse_key_value(const char *k, const char *v, atp_config_t *cfg) {
    if (strcmp(k, "PROXY_TCP_PORT") == 0) cfg->tcp_port = atoi(v);
    else if (strcmp(k, "PROXY_UDP_PORT") == 0) cfg->udp_port = atoi(v);
    else if (strcmp(k, "REDIRECT_TCP_PORT") == 0) cfg->redirect_tcp_port = atoi(v);
    else if (strcmp(k, "PROXY_MODE") == 0) cfg->proxy_mode = atoi(v);
    else if (strcmp(k, "PERFORMANCE_MODE") == 0) cfg->performance_mode = atoi(v);
    else if (strcmp(k, "PROXY_TCP") == 0) cfg->proxy_tcp = atoi(v);
    else if (strcmp(k, "PROXY_UDP") == 0) cfg->proxy_udp = atoi(v);
    else if (strcmp(k, "PROXY_IPV6") == 0) cfg->proxy_ipv6 = atoi(v);
    else if (strcmp(k, "SKIP_CHECK_FEATURE") == 0) cfg->skip_check_feature = atoi(v);
    else if (strcmp(k, "DNS_HIJACK_ENABLE") == 0) cfg->dns_hijack = atoi(v);
    else if (strcmp(k, "DNS_PORT") == 0) cfg->dns_port = atoi(v);
    else if (strcmp(k, "MARK_VALUE") == 0) cfg->mark_value = atoi(v);
    else if (strcmp(k, "MARK_VALUE6") == 0) cfg->mark_value6 = atoi(v);
    else if (strcmp(k, "TABLE_ID") == 0) cfg->table_id = atoi(v);
    else if (strcmp(k, "ROUTING_MARK") == 0) snprintf(cfg->routing_mark, sizeof(cfg->routing_mark), "%s", v);
    else if (strcmp(k, "FORCE_MARK_BYPASS") == 0) cfg->force_mark_bypass = atoi(v);
    else if (strcmp(k, "MOBILE_INTERFACE") == 0) snprintf(cfg->mobile_iface, sizeof(cfg->mobile_iface), "%s", v);
    else if (strcmp(k, "WIFI_INTERFACE") == 0) snprintf(cfg->wifi_iface, sizeof(cfg->wifi_iface), "%s", v);
    else if (strcmp(k, "HOTSPOT_INTERFACE") == 0) snprintf(cfg->hotspot_iface, sizeof(cfg->hotspot_iface), "%s", v);
    else if (strcmp(k, "USB_INTERFACE") == 0) snprintf(cfg->usb_iface, sizeof(cfg->usb_iface), "%s", v);
    else if (strcmp(k, "OTHER_BYPASS_INTERFACES") == 0) snprintf(cfg->other_bypass, sizeof(cfg->other_bypass), "%s", v);
    else if (strcmp(k, "OTHER_PROXY_INTERFACES") == 0) snprintf(cfg->other_proxy, sizeof(cfg->other_proxy), "%s", v);
    else if (strcmp(k, "PROXY_MOBILE") == 0) cfg->proxy_mobile = atoi(v);
    else if (strcmp(k, "PROXY_WIFI") == 0) cfg->proxy_wifi = atoi(v);
    else if (strcmp(k, "PROXY_HOTSPOT") == 0) cfg->proxy_hotspot = atoi(v);
    else if (strcmp(k, "PROXY_USB") == 0) cfg->proxy_usb = atoi(v);
    else if (strcmp(k, "HOTSPOT_SUBNET_IPV4") == 0) snprintf(cfg->hotspot_subnet_ipv4, sizeof(cfg->hotspot_subnet_ipv4), "%s", v);
    else if (strcmp(k, "HOTSPOT_SUBNET_IPV6") == 0) snprintf(cfg->hotspot_subnet_ipv6, sizeof(cfg->hotspot_subnet_ipv6), "%s", v);
    else if (strcmp(k, "PROXY_IPv4_LIST") == 0) snprintf(cfg->proxy_ipv4_list, sizeof(cfg->proxy_ipv4_list), "%s", v);
    else if (strcmp(k, "PROXY_IPv6_LIST") == 0) snprintf(cfg->proxy_ipv6_list, sizeof(cfg->proxy_ipv6_list), "%s", v);
    else if (strcmp(k, "BYPASS_IPv4_LIST") == 0) snprintf(cfg->bypass_ipv4_list, sizeof(cfg->bypass_ipv4_list), "%s", v);
    else if (strcmp(k, "BYPASS_IPv6_LIST") == 0) snprintf(cfg->bypass_ipv6_list, sizeof(cfg->bypass_ipv6_list), "%s", v);
    else if (strcmp(k, "BYPASS_CN_IP") == 0) cfg->bypass_cn_ip = atoi(v);
    else if (strcmp(k, "CN_IP_FILE") == 0) snprintf(cfg->cn_ip_file, sizeof(cfg->cn_ip_file), "%s", v);
    else if (strcmp(k, "CN_IPV6_FILE") == 0) snprintf(cfg->cn_ipv6_file, sizeof(cfg->cn_ipv6_file), "%s", v);
    else if (strcmp(k, "CN_IP_URL") == 0) snprintf(cfg->cn_ip_url, sizeof(cfg->cn_ip_url), "%s", v);
    else if (strcmp(k, "CN_IPV6_URL") == 0) snprintf(cfg->cn_ipv6_url, sizeof(cfg->cn_ipv6_url), "%s", v);
    else if (strcmp(k, "APP_PROXY_ENABLE") == 0) cfg->app_proxy_enable = atoi(v);
    else if (strcmp(k, "PROXY_APPS_LIST") == 0) snprintf(cfg->proxy_apps_list, sizeof(cfg->proxy_apps_list), "%s", v);
    else if (strcmp(k, "BYPASS_APPS_LIST") == 0) snprintf(cfg->bypass_apps_list, sizeof(cfg->bypass_apps_list), "%s", v);
    else if (strcmp(k, "APP_PROXY_MODE") == 0) snprintf(cfg->app_proxy_mode, sizeof(cfg->app_proxy_mode), "%s", v);
    else if (strcmp(k, "MAC_FILTER_ENABLE") == 0) cfg->mac_filter_enable = atoi(v);
    else if (strcmp(k, "PROXY_MACS_LIST") == 0) snprintf(cfg->proxy_macs_list, sizeof(cfg->proxy_macs_list), "%s", v);
    else if (strcmp(k, "BYPASS_MACS_LIST") == 0) snprintf(cfg->bypass_macs_list, sizeof(cfg->bypass_macs_list), "%s", v);
    else if (strcmp(k, "MAC_PROXY_MODE") == 0) snprintf(cfg->mac_proxy_mode, sizeof(cfg->mac_proxy_mode), "%s", v);
    else if (strcmp(k, "BLOCK_QUIC") == 0) cfg->block_quic = atoi(v);
    else if (strcmp(k, "LOG_TIMESTAMP") == 0) cfg->log_timestamp = atoi(v);
    else if (strcmp(k, "USER_CLASH_MODE") == 0) snprintf(cfg->user_clash_mode, sizeof(cfg->user_clash_mode), "%s", v);
    else if (strcmp(k, "RESTART_DELAY") == 0) cfg->restart_delay = atoi(v);
    else if (strcmp(k, "CLASH_SECRET") == 0) snprintf(cfg->clash_secret, sizeof(cfg->clash_secret), "%s", v);
    else if (strcmp(k, "API_PORT") == 0) cfg->api_port = atoi(v);
    else if (strcmp(k, "API_HOST") == 0) snprintf(cfg->api_host, sizeof(cfg->api_host), "%s", v);
    else if (strcmp(k, "UI_EMOJI_ENABLED") == 0) cfg->ui_emoji_enabled = atoi(v);
    else if (strcmp(k, "CORE_USER_GROUP") == 0) {
        char val[256]; snprintf(val, sizeof(val), "%s", v); char *colon = strchr(val, ':');
        if (colon) { 
            *colon = '\0'; 
            snprintf(cfg->core_user, sizeof(cfg->core_user), "%.63s", val); 
            snprintf(cfg->core_group, sizeof(cfg->core_group), "%.63s", colon + 1); 
        }
    }
}

int config_load(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    FILE *fp = fopen(path, "r"); if (!fp) { pthread_mutex_unlock(&cfg->config_mutex); return -1; }
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
    fclose(fp); pthread_mutex_unlock(&cfg->config_mutex);
    LOG_INFO("Configuration loaded: %s", path); return 0;
}

int config_set_mode(atp_config_t *cfg, const char *mode) {
    pthread_mutex_lock(&cfg->config_mutex);
    snprintf(cfg->user_clash_mode, sizeof(cfg->user_clash_mode), "%s", mode);
    char data_dir[SAFE_PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s", cfg->data_dir);
    pthread_mutex_unlock(&cfg->config_mutex);

    char cp[SAFE_PATH_MAX], tp[SAFE_PATH_MAX];
    if (snprintf(cp, sizeof(cp), "%s/%s", data_dir, ATP_CONF_FILE) >= (int)sizeof(cp)) return -1;
    if (!file_exists(cp)) return 0;
    if (snprintf(tp, sizeof(tp), "%s.tmp", cp) >= (int)sizeof(tp)) return -1;
    FILE *fin = fopen(cp, "r"), *fout = fopen(tp, "w");
    if (!fin || !fout) { if (fin) fclose(fin); if (fout) fclose(fout); return -1; }
    char line[1024]; int found = 0;
    while (fgets(line, sizeof(line), fin)) {
        if (strncmp(line, "USER_CLASH_MODE=", 16) == 0) { fprintf(fout, "USER_CLASH_MODE=\"%s\"\n", mode); found = 1; }
        else fputs(line, fout);
    }
    if (!found) fprintf(fout, "USER_CLASH_MODE=\"%s\"\n", mode);
    fclose(fin); fclose(fout); return rename(tp, cp);
}

int config_reload(atp_config_t *cfg) {
    char cp[SAFE_PATH_MAX];
    if (snprintf(cp, sizeof(cp), "%s/%s", cfg->data_dir, ATP_CONF_FILE) >= (int)sizeof(cp)) return -1;
    return file_exists(cp) ? config_load(cp, cfg) : -1;
}

int config_save_runtime(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    FILE *fp = fopen(path, "w"); if (!fp) { pthread_mutex_unlock(&cfg->config_mutex); return -1; }
    fprintf(fp, "# ATP Runtime\nUSE_TPROXY=%d\nTABLE_ID=%d\nMARK_VALUE=%d\n", 
            cfg->use_tproxy, cfg->table_id, cfg->mark_value);
    fclose(fp); pthread_mutex_unlock(&cfg->config_mutex); return 0;
}

int validate_interface_name(const char *n) {
    if (!n || !*n || strlen(n) > 15) return -1;
    for (const char *p = n; *p; p++) if (!isalnum(*p) && !strchr("_+-.", *p)) return -1;
    return 0;
}

int validate_port(int p) { return p > 0 && p <= 65535; }
int validate_mark(int m) { return m >= 1 && m <= 2147483647; }
