/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Configuration validator implementation
 */

#include "config_validator.h"
#include "logger.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>

/* Known configuration keys */
static const char *VALID_CONFIG_KEYS[] = {
    "PROXY_TCP_PORT",
    "PROXY_UDP_PORT",
    "REDIRECT_TCP_PORT",
    "PROXY_MODE",
    "PERFORMANCE_MODE",
    "PROXY_TCP",
    "PROXY_UDP",
    "PROXY_IPV6",
    "SKIP_CHECK_FEATURE",
    "DNS_HIJACK_ENABLE",
    "DNS_PORT",
    "MARK_VALUE",
    "MARK_VALUE6",
    "TABLE_ID",
    "ROUTING_MARK",
    "FORCE_MARK_BYPASS",
    "MOBILE_INTERFACE",
    "WIFI_INTERFACE",
    "HOTSPOT_INTERFACE",
    "USB_INTERFACE",
    "OTHER_BYPASS_INTERFACES",
    "OTHER_PROXY_INTERFACES",
    "PROXY_MOBILE",
    "PROXY_WIFI",
    "PROXY_HOTSPOT",
    "PROXY_USB",
    "HOTSPOT_SUBNET_IPV4",
    "HOTSPOT_SUBNET_IPV6",
    "PROXY_IPv4_LIST",
    "PROXY_IPv6_LIST",
    "BYPASS_IPv4_LIST",
    "BYPASS_IPv6_LIST",
    "BYPASS_CN_IP",
    "CN_IP_FILE",
    "CN_IPV6_FILE",
    "CN_IP_URL",
    "CN_IPV6_URL",
    "APP_PROXY_ENABLE",
    "PROXY_APPS_LIST",
    "BYPASS_APPS_LIST",
    "APP_PROXY_MODE",
    "MAC_FILTER_ENABLE",
    "PROXY_MACS_LIST",
    "BYPASS_MACS_LIST",
    "MAC_PROXY_MODE",
    "BLOCK_QUIC",
    "LOG_TIMESTAMP",
    "USER_CLASH_MODE",
    "RESTART_DELAY",
    "CLASH_SECRET",
    "API_PORT",
    "API_HOST",
    "UI_EMOJI_ENABLED",
    "CORE_USER_GROUP",
    NULL
};

static int g_strict_mode = 0;

void config_set_strict_mode(int strict) {
    g_strict_mode = strict;
}

int config_get_strict_mode(void) {
    return g_strict_mode;
}

static int is_valid_key(const char *key) {
    for (int i = 0; VALID_CONFIG_KEYS[i] != NULL; i++) {
        if (strcmp(key, VALID_CONFIG_KEYS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int levenshtein_distance(const char *s1, const char *s2) {
    int len1 = strlen(s1);
    int len2 = strlen(s2);
    int d[len1 + 1][len2 + 1];
    
    for (int i = 0; i <= len1; i++) d[i][0] = i;
    for (int j = 0; j <= len2; j++) d[0][j] = j;
    
    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            int min = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < min) min = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < min) min = d[i - 1][j - 1] + cost;
            d[i][j] = min;
        }
    }
    
    return d[len1][len2];
}

static void find_best_suggestion(const char *input, char *suggestion, size_t size) {
    int min_dist = 999;
    const char *best_match = NULL;
    
    for (int i = 0; VALID_CONFIG_KEYS[i] != NULL; i++) {
        int dist = levenshtein_distance(input, VALID_CONFIG_KEYS[i]);
        if (dist < min_dist && dist <= 4) {
            min_dist = dist;
            best_match = VALID_CONFIG_KEYS[i];
        }
    }
    
    if (best_match) {
        strncpy(suggestion, best_match, size - 1);
        suggestion[size - 1] = '\0';
    } else {
        suggestion[0] = '\0';
    }
}

int config_validate_key(const char *key, atp_config_t *cfg) {
    (void)cfg;
    
    if (is_valid_key(key)) {
        return 0;
    }
    
    char suggestion[64] = {0};
    find_best_suggestion(key, suggestion, sizeof(suggestion));
    
    if (suggestion[0]) {
        LOG_WARN("Unknown config key: '%s'. Did you mean '%s'?", key, suggestion);
    } else {
        LOG_WARN("Unknown config key: '%s'", key);
    }
    
    return g_strict_mode ? -1 : 0;
}

static int validate_port(int port, const char *name) {
    if (port < 1 || port > 65535) {
        LOG_ERROR("%s must be between 1-65535, got %d", name, port);
        return -1;
    }
    return 0;
}

static int validate_proxy_mode(int mode) {
    if (mode < 0 || mode > 3) {
        LOG_ERROR("PROXY_MODE must be 0-3 (0=auto,1=tproxy,2=redirect,3=enhance), got %d", mode);
        return -1;
    }
    return 0;
}

static int validate_table_id(int id) {
    if (id < 1 || id > 252) {
        LOG_ERROR("TABLE_ID must be between 1-252, got %d", id);
        return -1;
    }
    return 0;
}

static int validate_mark_value(int mark, const char *name) {
    if (mark < 1 || mark > 2147483647) {
        LOG_ERROR("%s must be between 1-2147483647, got %d", name, mark);
        return -1;
    }
    return 0;
}

static int validate_app_proxy_mode(const char *mode) {
    if (mode && mode[0]) {
        if (strcmp(mode, "blacklist") != 0 && strcmp(mode, "whitelist") != 0) {
            LOG_ERROR("APP_PROXY_MODE must be 'blacklist' or 'whitelist', got '%s'", mode);
            return -1;
        }
    }
    return 0;
}
static int validate_mac_proxy_mode(const char *mode) {
    if (mode && mode[0]) {
        if (strcmp(mode, "blacklist") != 0 && strcmp(mode, "whitelist") != 0) {
            LOG_ERROR("MAC_PROXY_MODE must be 'blacklist' or 'whitelist', got '%s'", mode);
            return -1;
        }
    }
    return 0;
}

static int validate_interface_name(const char *name, const char *field) {
    if (!name || !*name) return 0;

    for (const char *p = name; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') &&
            !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') &&
            *p != '_' && *p != '-' && *p != '+' && *p != '.') {
            LOG_ERROR("%s contains invalid character '%c'", field, *p);
            return -1;
        }
    }
    return 0;
}

static int validate_user_group(const char *user_group) {
    if (!user_group || !*user_group) return 0;

    char tmp[128];
    strncpy(tmp, user_group, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *colon = strchr(tmp, ':');
    if (!colon) {
        LOG_ERROR("CORE_USER_GROUP must be in format 'user:group', got '%s'", user_group);
        return -1;
    }

    *colon = '\0';
    char *user = tmp;
    char *group = colon + 1;

    struct passwd *pwd = getpwnam(user);
    if (!pwd) {
        LOG_WARN("User '%s' does not exist on system", user);
    }

    struct group *grp = getgrnam(group);
    if (!grp) {
        LOG_WARN("Group '%s' does not exist on system", group);
    }

    return 0;
}

int config_validate_values(atp_config_t *cfg) {
    int errors = 0;

    errors += validate_port(cfg->network.tcp_port, "PROXY_TCP_PORT") ? 1 : 0;
    errors += validate_port(cfg->network.udp_port, "PROXY_UDP_PORT") ? 1 : 0;
    errors += validate_port(cfg->network.redirect_tcp_port, "REDIRECT_TCP_PORT") ? 1 : 0;
    errors += validate_port(cfg->network.dns_port, "DNS_PORT") ? 1 : 0;
    errors += validate_port(cfg->api.port, "API_PORT") ? 1 : 0;

    if (cfg->network.tcp_port == cfg->network.redirect_tcp_port) {
        LOG_ERROR("PROXY_TCP_PORT and REDIRECT_TCP_PORT cannot be the same (%d)",
                  cfg->network.tcp_port);
        errors++;
    }

    if (cfg->network.udp_port == cfg->network.dns_port && cfg->network.dns_hijack) {
        LOG_WARN("PROXY_UDP_PORT and DNS_PORT are the same (%d) - DNS may not work correctly",
                 cfg->network.udp_port);
    }

    errors += validate_proxy_mode(cfg->network.proxy_mode) ? 1 : 0;
    errors += validate_table_id(cfg->network.table_id) ? 1 : 0;
    errors += validate_mark_value(cfg->network.mark_value, "MARK_VALUE") ? 1 : 0;
    errors += validate_mark_value(cfg->network.mark_value6, "MARK_VALUE6") ? 1 : 0;

    if (cfg->filter.app_proxy_enable) {
        errors += validate_app_proxy_mode(cfg->filter.app_proxy_mode) ? 1 : 0;
    }
    if (cfg->filter.mac_filter_enable) {
        errors += validate_mac_proxy_mode(cfg->filter.mac_proxy_mode) ? 1 : 0;
    }

    errors += validate_interface_name(cfg->interface.mobile_iface, "MOBILE_INTERFACE") ? 1 : 0;
    errors += validate_interface_name(cfg->interface.wifi_iface, "WIFI_INTERFACE") ? 1 : 0;
    errors += validate_interface_name(cfg->interface.hotspot_iface, "HOTSPOT_INTERFACE") ? 1 : 0;
    errors += validate_interface_name(cfg->interface.usb_iface, "USB_INTERFACE") ? 1 : 0;

    char user_group[128];
    snprintf(user_group, sizeof(user_group), "%s:%s", cfg->core.core_user, cfg->core.core_group);
    errors += validate_user_group(user_group) ? 1 : 0;

    if (cfg->core.restart_delay < 0 || cfg->core.restart_delay > 3600) {
        LOG_ERROR("RESTART_DELAY must be between 0-3600 seconds, got %d", cfg->core.restart_delay);
        errors++;
    }

    if (errors > 0) {
        LOG_ERROR("Configuration validation found %d error(s)", errors);
        return -1;
    }

    return 0;
}	
