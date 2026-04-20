#include "config.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <pwd.h>
#include <grp.h>
#include <arpa/inet.h>

void config_set_defaults(atp_config_t *cfg) {
    memset(cfg, 0, sizeof(atp_config_t));
    
    /* Initialize mutex */
    pthread_mutex_init(&cfg->config_mutex, NULL);
    
    strcpy(cfg->data_dir, ATP_DEFAULT_DIR);
    cfg->dry_run = 0;
    cfg->verbose = 0;
    cfg->foreground = 0;
    
    cfg->tcp_port = DEFAULT_TCP_PORT;
    cfg->udp_port = DEFAULT_UDP_PORT;
    cfg->redirect_tcp_port = DEFAULT_REDIRECT_TCP_PORT;
    cfg->proxy_mode = MODE_AUTO;
    cfg->performance_mode = 0;
    cfg->proxy_tcp = 1;
    cfg->proxy_udp = 1;
    cfg->proxy_ipv6 = 0;
    
    cfg->dns_hijack = DNS_HIJACK_TPROXY;
    cfg->dns_port = DEFAULT_DNS_PORT;
    
    cfg->mark_value = DEFAULT_MARK;
    cfg->mark_value6 = DEFAULT_MARK6;
    cfg->table_id = DEFAULT_TABLE_ID;
    strcpy(cfg->core_user, "root");
    strcpy(cfg->core_group, "net_admin");
    cfg->force_mark_bypass = 0;
    cfg->routing_mark[0] = '\0';
    
    strcpy(cfg->mobile_iface, "rmnet_data+");
    strcpy(cfg->wifi_iface, "wlan0");
    strcpy(cfg->hotspot_iface, "wlan2");
    strcpy(cfg->usb_iface, "rndis+");
    cfg->proxy_mobile = 1;
    cfg->proxy_wifi = 1;
    cfg->proxy_hotspot = 0;
    cfg->proxy_usb = 0;
    
    strcpy(cfg->hotspot_subnet_ipv4, "192.168.43.0/24");
    strcpy(cfg->hotspot_subnet_ipv6, "fe80::/10");
    
    strcpy(cfg->proxy_ipv4_list, "");
    strcpy(cfg->proxy_ipv6_list, "");
    strcpy(cfg->bypass_ipv4_list, "");
    strcpy(cfg->bypass_ipv6_list, "");
    
    cfg->bypass_cn_ip = 0;
    strcpy(cfg->cn_ip_file, "cn.zone");
    strcpy(cfg->cn_ipv6_file, "cn_ipv6.zone");
    strcpy(cfg->cn_ip_url, "https://raw.githubusercontent.com/Hackl0us/GeoIP2-CN/release/CN-ip-cidr.txt");
    strcpy(cfg->cn_ipv6_url, "https://ispip.clang.cn/all_cn_ipv6.txt");
    
    cfg->app_proxy_enable = 0;
    strcpy(cfg->proxy_apps_list, "");
    strcpy(cfg->bypass_apps_list, "");
    strcpy(cfg->app_proxy_mode, "blacklist");
    
    cfg->mac_filter_enable = 0;
    strcpy(cfg->proxy_macs_list, "");
    strcpy(cfg->bypass_macs_list, "");
    strcpy(cfg->mac_proxy_mode, "blacklist");
    
    cfg->block_quic = 0;
    cfg->log_timestamp = 1;
    cfg->skip_check_feature = 0;
    strcpy(cfg->user_clash_mode, "Rule");
    cfg->restart_delay = DEFAULT_RESTART_DELAY;
    cfg->clash_secret[0] = '\0';
    
    /* API defaults */
    cfg->api_port = DEFAULT_API_PORT;
    strcpy(cfg->api_host, DEFAULT_API_HOST);
    
    cfg->use_tproxy = 0;
    cfg->current_vpn_iface[0] = '\0';
    /* UI defaults */
    cfg->ui_emoji_enabled = 1;   /* Emoji ON by default */
}

static void parse_key_value(const char *key, const char *value, atp_config_t *cfg) {
    if (strcmp(key, "PROXY_TCP_PORT") == 0) cfg->tcp_port = atoi(value);
    else if (strcmp(key, "PROXY_UDP_PORT") == 0) cfg->udp_port = atoi(value);
    else if (strcmp(key, "REDIRECT_TCP_PORT") == 0) cfg->redirect_tcp_port = atoi(value);
    else if (strcmp(key, "PROXY_MODE") == 0) cfg->proxy_mode = atoi(value);
    else if (strcmp(key, "PERFORMANCE_MODE") == 0) cfg->performance_mode = atoi(value);
    else if (strcmp(key, "PROXY_TCP") == 0) cfg->proxy_tcp = atoi(value);
    else if (strcmp(key, "PROXY_UDP") == 0) cfg->proxy_udp = atoi(value);
    else if (strcmp(key, "PROXY_IPV6") == 0) cfg->proxy_ipv6 = atoi(value);
    else if (strcmp(key, "DNS_HIJACK_ENABLE") == 0) cfg->dns_hijack = atoi(value);
    else if (strcmp(key, "DNS_PORT") == 0) cfg->dns_port = atoi(value);
    else if (strcmp(key, "MARK_VALUE") == 0) cfg->mark_value = atoi(value);
    else if (strcmp(key, "MARK_VALUE6") == 0) cfg->mark_value6 = atoi(value);
    else if (strcmp(key, "TABLE_ID") == 0) cfg->table_id = atoi(value);
    else if (strcmp(key, "ROUTING_MARK") == 0) strcpy(cfg->routing_mark, value);
    else if (strcmp(key, "FORCE_MARK_BYPASS") == 0) cfg->force_mark_bypass = atoi(value);
    else if (strcmp(key, "MOBILE_INTERFACE") == 0) strcpy(cfg->mobile_iface, value);
    else if (strcmp(key, "WIFI_INTERFACE") == 0) strcpy(cfg->wifi_iface, value);
    else if (strcmp(key, "HOTSPOT_INTERFACE") == 0) strcpy(cfg->hotspot_iface, value);
    else if (strcmp(key, "USB_INTERFACE") == 0) strcpy(cfg->usb_iface, value);
    else if (strcmp(key, "OTHER_BYPASS_INTERFACES") == 0) strcpy(cfg->other_bypass, value);
    else if (strcmp(key, "OTHER_PROXY_INTERFACES") == 0) strcpy(cfg->other_proxy, value);
    else if (strcmp(key, "PROXY_MOBILE") == 0) cfg->proxy_mobile = atoi(value);
    else if (strcmp(key, "PROXY_WIFI") == 0) cfg->proxy_wifi = atoi(value);
    else if (strcmp(key, "PROXY_HOTSPOT") == 0) cfg->proxy_hotspot = atoi(value);
    else if (strcmp(key, "PROXY_USB") == 0) cfg->proxy_usb = atoi(value);
    else if (strcmp(key, "HOTSPOT_SUBNET_IPV4") == 0) strcpy(cfg->hotspot_subnet_ipv4, value);
    else if (strcmp(key, "HOTSPOT_SUBNET_IPV6") == 0) strcpy(cfg->hotspot_subnet_ipv6, value);
    else if (strcmp(key, "PROXY_IPv4_LIST") == 0) strcpy(cfg->proxy_ipv4_list, value);
    else if (strcmp(key, "PROXY_IPv6_LIST") == 0) strcpy(cfg->proxy_ipv6_list, value);
    else if (strcmp(key, "BYPASS_IPv4_LIST") == 0) strcpy(cfg->bypass_ipv4_list, value);
    else if (strcmp(key, "BYPASS_IPv6_LIST") == 0) strcpy(cfg->bypass_ipv6_list, value);
    else if (strcmp(key, "BYPASS_CN_IP") == 0) cfg->bypass_cn_ip = atoi(value);
    else if (strcmp(key, "CN_IP_FILE") == 0) strcpy(cfg->cn_ip_file, value);
    else if (strcmp(key, "CN_IPV6_FILE") == 0) strcpy(cfg->cn_ipv6_file, value);
    else if (strcmp(key, "CN_IP_URL") == 0) strcpy(cfg->cn_ip_url, value);
    else if (strcmp(key, "CN_IPV6_URL") == 0) strcpy(cfg->cn_ipv6_url, value);
    else if (strcmp(key, "APP_PROXY_ENABLE") == 0) cfg->app_proxy_enable = atoi(value);
    else if (strcmp(key, "PROXY_APPS_LIST") == 0) strcpy(cfg->proxy_apps_list, value);
    else if (strcmp(key, "BYPASS_APPS_LIST") == 0) strcpy(cfg->bypass_apps_list, value);
    else if (strcmp(key, "APP_PROXY_MODE") == 0) strcpy(cfg->app_proxy_mode, value);
    else if (strcmp(key, "MAC_FILTER_ENABLE") == 0) cfg->mac_filter_enable = atoi(value);
    else if (strcmp(key, "PROXY_MACS_LIST") == 0) strcpy(cfg->proxy_macs_list, value);
    else if (strcmp(key, "BYPASS_MACS_LIST") == 0) strcpy(cfg->bypass_macs_list, value);
    else if (strcmp(key, "MAC_PROXY_MODE") == 0) strcpy(cfg->mac_proxy_mode, value);
    else if (strcmp(key, "BLOCK_QUIC") == 0) cfg->block_quic = atoi(value);
    else if (strcmp(key, "LOG_TIMESTAMP") == 0) cfg->log_timestamp = atoi(value);
    else if (strcmp(key, "SKIP_CHECK_FEATURE") == 0) cfg->skip_check_feature = atoi(value);
    else if (strcmp(key, "USER_CLASH_MODE") == 0) strcpy(cfg->user_clash_mode, value);
    else if (strcmp(key, "RESTART_DELAY") == 0) cfg->restart_delay = atoi(value);
    else if (strcmp(key, "CLASH_SECRET") == 0) strcpy(cfg->clash_secret, value);
    else if (strcmp(key, "API_PORT") == 0) cfg->api_port = atoi(value);
    else if (strcmp(key, "API_HOST") == 0) strcpy(cfg->api_host, value);
    else if (strcmp(key, "UI_EMOJI_ENABLED") == 0) cfg->ui_emoji_enabled = atoi(value);
    else if (strcmp(key, "CORE_USER_GROUP") == 0) {
        char *colon = strchr(value, ':');
        if (colon) {
            *colon = '\0';
            strcpy(cfg->core_user, value);
            strcpy(cfg->core_group, colon + 1);
        }
    }
}

int config_load(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN("Cannot open config file: %s", path);
        pthread_mutex_unlock(&cfg->config_mutex);
        return -1;
    }
    
    char line[1024];
    char key[512], value[512];
    
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        strcpy(key, line);
        strcpy(value, eq + 1);
        trim(key);
        trim(value);
        
        if ((value[0] == '"' && value[strlen(value)-1] == '"') ||
            (value[0] == '\'' && value[strlen(value)-1] == '\'')) {
            memmove(value, value + 1, strlen(value) - 2);
            value[strlen(value) - 2] = '\0';
        }
        
        parse_key_value(key, value, cfg);
    }
    
    fclose(fp);
    pthread_mutex_unlock(&cfg->config_mutex);
    
    LOG_INFO("Configuration loaded from %s", path);
    return 0;
}

int config_save(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    
    FILE *fp = fopen(path, "w");
    if (!fp) {
        pthread_mutex_unlock(&cfg->config_mutex);
        return -1;
    }
    
    fprintf(fp, "# ATP Configuration File\n");
    fprintf(fp, "# Generated by atpd v" ATP_VERSION_STRING "\n\n");
    
    fprintf(fp, "PROXY_TCP_PORT=%d\n", cfg->tcp_port);
    fprintf(fp, "PROXY_UDP_PORT=%d\n", cfg->udp_port);
    fprintf(fp, "REDIRECT_TCP_PORT=%d\n", cfg->redirect_tcp_port);
    fprintf(fp, "PROXY_MODE=%d\n", cfg->proxy_mode);
    fprintf(fp, "PERFORMANCE_MODE=%d\n", cfg->performance_mode);
    fprintf(fp, "PROXY_IPV6=%d\n", cfg->proxy_ipv6);
    fprintf(fp, "DNS_HIJACK_ENABLE=%d\n", cfg->dns_hijack);
    fprintf(fp, "DNS_PORT=%d\n", cfg->dns_port);
    fprintf(fp, "MARK_VALUE=%d\n", cfg->mark_value);
    fprintf(fp, "MARK_VALUE6=%d\n", cfg->mark_value6);
    fprintf(fp, "TABLE_ID=%d\n", cfg->table_id);
    fprintf(fp, "BYPASS_CN_IP=%d\n", cfg->bypass_cn_ip);
    fprintf(fp, "BLOCK_QUIC=%d\n", cfg->block_quic);
    fprintf(fp, "USER_CLASH_MODE=%s\n", cfg->user_clash_mode);
    fprintf(fp, "API_HOST=%s\n", cfg->api_host);
    fprintf(fp, "API_PORT=%d\n", cfg->api_port);
    
    fclose(fp);
    pthread_mutex_unlock(&cfg->config_mutex);
    return 0;
}

int config_save_runtime(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    
    FILE *fp = fopen(path, "w");
    if (!fp) {
        pthread_mutex_unlock(&cfg->config_mutex);
        return -1;
    }
    
    fprintf(fp, "# ATP Runtime Config Snapshot\n");
    fprintf(fp, "ATP_DATA=%s\n", cfg->data_dir);
    fprintf(fp, "USE_TPROXY=%d\n", cfg->use_tproxy);
    fprintf(fp, "PROXY_IPV6=%d\n", cfg->proxy_ipv6);
    fprintf(fp, "TABLE_ID=%d\n", cfg->table_id);
    fprintf(fp, "MARK_VALUE=%d\n", cfg->mark_value);
    fprintf(fp, "MARK_VALUE6=%d\n", cfg->mark_value6);
    fprintf(fp, "USER_CLASH_MODE=%s\n", cfg->user_clash_mode);
    
    fclose(fp);
    pthread_mutex_unlock(&cfg->config_mutex);
    
    LOG_DEBUG("Runtime config saved to %s", path);
    return 0;
}

int config_load_runtime(const char *path, atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        pthread_mutex_unlock(&cfg->config_mutex);
        return -1;
    }
    
    char line[512];
    char key[256], value[256];
    
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        strcpy(key, line);
        strcpy(value, eq + 1);
        trim(key);
        trim(value);
        
        if (strcmp(key, "USE_TPROXY") == 0) cfg->use_tproxy = atoi(value);
        else if (strcmp(key, "PROXY_IPV6") == 0) cfg->proxy_ipv6 = atoi(value);
        else if (strcmp(key, "TABLE_ID") == 0) cfg->table_id = atoi(value);
        else if (strcmp(key, "MARK_VALUE") == 0) cfg->mark_value = atoi(value);
        else if (strcmp(key, "MARK_VALUE6") == 0) cfg->mark_value6 = atoi(value);
        else if (strcmp(key, "USER_CLASH_MODE") == 0) strcpy(cfg->user_clash_mode, value);
    }
    
    fclose(fp);
    pthread_mutex_unlock(&cfg->config_mutex);
    
    LOG_DEBUG("Runtime config loaded from %s", path);
    return 0;
}

int config_set_mode(atp_config_t *cfg, const char *mode) {
    pthread_mutex_lock(&cfg->config_mutex);
    
    strncpy(cfg->user_clash_mode, mode, sizeof(cfg->user_clash_mode) - 1);
    cfg->user_clash_mode[sizeof(cfg->user_clash_mode) - 1] = '\0';
    
    pthread_mutex_unlock(&cfg->config_mutex);
    
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/%s", cfg->data_dir, ATP_CONF_FILE);
    
    if (!file_exists(conf_path)) return 0;
    
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", conf_path);
    
    FILE *fin = fopen(conf_path, "r");
    FILE *fout = fopen(temp_path, "w");
    
    if (!fin || !fout) {
        if (fin) fclose(fin);
        if (fout) fclose(fout);
        return -1;
    }
    
    char line[1024];
    int replaced = 0;
    
    while (fgets(line, sizeof(line), fin)) {
        if (strncmp(line, "USER_CLASH_MODE=", 16) == 0) {
            fprintf(fout, "USER_CLASH_MODE=\"%s\"\n", mode);
            replaced = 1;
        } else {
            fputs(line, fout);
        }
    }
    
    if (!replaced) {
        fprintf(fout, "USER_CLASH_MODE=\"%s\"\n", mode);
    }
    
    fclose(fin);
    fclose(fout);
    
    rename(temp_path, conf_path);
    return 0;
}

int config_reload(atp_config_t *cfg) {
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/%s", cfg->data_dir, ATP_CONF_FILE);
    
    if (file_exists(conf_path)) {
        return config_load(conf_path, cfg);
    }
    
    LOG_WARN("Config file not found, keeping current settings");
    return -1;
}

void config_print_summary(atp_config_t *cfg) {
    pthread_mutex_lock(&cfg->config_mutex);
    
    LOG_INFO("Config Summary:");
    LOG_INFO("  Proxy Mode: %d (0=auto,1=tproxy,2=redirect,3=enhance)", cfg->proxy_mode);
    LOG_INFO("  TCP Port: %d, UDP Port: %d, REDIRECT TCP Port: %d", 
             cfg->tcp_port, cfg->udp_port, cfg->redirect_tcp_port);
    LOG_INFO("  IPv6: %s", cfg->proxy_ipv6 ? "enabled" : "disabled");
    LOG_INFO("  Mark: %d (IPv4), %d (IPv6)", cfg->mark_value, cfg->mark_value6);
    LOG_INFO("  Table ID: %d", cfg->table_id);
    LOG_INFO("  DNS Hijack: %d, DNS Port: %d", cfg->dns_hijack, cfg->dns_port);
    LOG_INFO("  CN IP Bypass: %s", cfg->bypass_cn_ip ? "enabled" : "disabled");
    LOG_INFO("  QUIC Block: %s", cfg->block_quic ? "enabled" : "disabled");
    LOG_INFO("  Clash Mode: %s", cfg->user_clash_mode);
    LOG_INFO("  API: http://%s:%d", cfg->api_host, cfg->api_port);
    
    pthread_mutex_unlock(&cfg->config_mutex);
}

int validate_user_group(const char *user, const char *group) {
    struct passwd *pwd = getpwnam(user);
    if (!pwd) return -1;
    
    struct group *grp = getgrnam(group);
    if (!grp) return -1;
    
    return 0;
}

int validate_interface_name(const char *name) {
    if (!name || !*name) return -1;
    
    for (const char *p = name; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '+' && *p != '.') {
            return -1;
        }
    }
    
    return 0;
}

int validate_ip_cidr(const char *cidr) {
    char ip[64];
    int prefix;
    
    if (sscanf(cidr, "%63[^/]/%d", ip, &prefix) != 2) return -1;
    
    struct in_addr ipv4;
    struct in6_addr ipv6;
    
    if (inet_pton(AF_INET, ip, &ipv4) == 1) {
        if (prefix >= 0 && prefix <= 32) return 0;
    } else if (inet_pton(AF_INET6, ip, &ipv6) == 1) {
        if (prefix >= 0 && prefix <= 128) return 0;
    }
    
    return -1;
}

int validate_port(int port) {
    return port > 0 && port <= 65535;
}

int validate_mark(int mark) {
    return mark >= 1 && mark <= 2147483647;
}
