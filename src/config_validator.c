/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration validator implementation
 */

#include "config_validator.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <net/if.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>

#define MAX_KEY_LEN 128
#define MAX_SUGGESTION_KEY 64
#define MAX_LEVENSHTEIN_LEN 64

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
    "ENABLE_EBPF",
    "EBPF_LOAD_RETRY",
    "EBPF_LOAD_DELAY",
    "EBPF_BIN",
    "EBPF_PIN_DIR",
    "EBPF_STATE_DIR",
    "SERVICE_START_TIMEOUT",
    "SERVICE_STOP_TIMEOUT",
    "SERVICE_GRACE_PERIOD",
    "SERVICE_MAX_FAILURES",
    "SERVICE_CIRCUIT_THRESHOLD",
    "SERVICE_CIRCUIT_COOLDOWN",
    "SERVICE_HEALTH_CHECK_INTERVAL",
    "CNIP_FORCE_PROXY_APPS",
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
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);

    if (len1 > MAX_LEVENSHTEIN_LEN || len2 > MAX_LEVENSHTEIN_LEN) {
        return INT_MAX;
    }

    int d[MAX_LEVENSHTEIN_LEN + 1][MAX_LEVENSHTEIN_LEN + 1];
    int l1 = (int)len1;
    int l2 = (int)len2;

    for (int i = 0; i <= l1; i++) d[i][0] = i;
    for (int j = 0; j <= l2; j++) d[0][j] = j;

    for (int i = 1; i <= l1; i++) {
        for (int j = 1; j <= l2; j++) {
            int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            int min = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < min) min = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < min) min = d[i - 1][j - 1] + cost;
            d[i][j] = min;
        }
    }

    return d[l1][l2];
}

static void find_best_suggestion(const char *input, char *suggestion, size_t size) {
    int min_dist = INT_MAX;
    const char *best_match = NULL;

    if (!suggestion || size == 0) return;

    if (strlen(input) > MAX_KEY_LEN) {
        suggestion[0] = '\0';
        return;
    }

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

    if (!key || !*key) {
        return -1;
    }

    if (is_valid_key(key)) {
        return 0;
    }

    char suggestion[MAX_SUGGESTION_KEY] = {0};
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
    if (mode < MODE_AUTO || mode > MODE_ENHANCE) {
        LOG_ERROR("PROXY_MODE must be %d-%d (0=auto,1=tproxy,2=redirect,3=enhance), got %d",
                  MODE_AUTO, MODE_ENHANCE, mode);
        return -1;
    }
    return 0;
}

static int validate_table_id(int id) {
    if (id < 1 || id > 65535) {
        LOG_ERROR("TABLE_ID must be between 1-65535, got %d", id);
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

static int validate_clash_mode(const char *mode) {
    if (!mode || !*mode) {
        return 0;
    }

    if (strcmp(mode, "Rule") == 0) return 0;
    if (strcmp(mode, "Global") == 0) return 0;
    if (strcmp(mode, "Direct") == 0) return 0;
    if (strcmp(mode, "Google VPN") == 0) return 0;

    LOG_ERROR("USER_CLASH_MODE must be 'Rule', 'Global', 'Direct', or 'Google VPN', got '%s'", mode);
    return -1;
}

static int validate_bool(int value, const char *name) {
    if (value != 0 && value != 1) {
        LOG_ERROR("%s must be 0 or 1, got %d", name, value);
        return -1;
    }
    return 0;
}

static int validate_dns_hijack_mode(int mode) {
    if (mode != DNS_HIJACK_OFF && mode != DNS_HIJACK_TPROXY && mode != DNS_HIJACK_REDIRECT) {
        LOG_ERROR("DNS_HIJACK_ENABLE must be 0(off), 1(tproxy), or 2(redirect), got %d", mode);
        return -1;
    }
    return 0;
}

static int validate_interface_name(const char *name, const char *field) {
    if (!name || !*name) return 0;

    if (strlen(name) >= IFNAMSIZ) {
        LOG_ERROR("%s exceeds IFNAMSIZ (%d): %s", field, IFNAMSIZ - 1, name);
        return -1;
    }

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

    long pw_size = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (pw_size < 1024) pw_size = 1024;

    char *pwd_buf = malloc(pw_size);
    if (!pwd_buf) {
        LOG_ERROR("Failed to allocate memory for user lookup");
        return -1;
    }

    struct passwd pwd_result;
    struct passwd *pwd = NULL;
    int ret = getpwnam_r(user, &pwd_result, pwd_buf, pw_size, &pwd);
    if (ret != 0) {
        LOG_WARN("Failed to lookup user '%s': %s", user, strerror(ret));
    } else if (!pwd) {
        LOG_WARN("User '%s' does not exist on system", user);
    }

    long gr_size = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (gr_size < 1024) gr_size = 1024;

    char *grp_buf = malloc(gr_size);
    if (!grp_buf) {
        free(pwd_buf);
        LOG_ERROR("Failed to allocate memory for group lookup");
        return -1;
    }

    struct group grp_result;
    struct group *grp = NULL;
    ret = getgrnam_r(group, &grp_result, grp_buf, gr_size, &grp);
    if (ret != 0) {
        LOG_WARN("Failed to lookup group '%s': %s", group, strerror(ret));
    } else if (!grp) {
        LOG_WARN("Group '%s' does not exist on system", group);
    }

    free(pwd_buf);
    free(grp_buf);

    return 0;
}

static int validate_ebpf_params(atp_config_t *cfg) {
    int errors = 0;

    if (cfg->ebpf.load_retry < 0 || cfg->ebpf.load_retry > 100) {
        LOG_ERROR("EBPF_LOAD_RETRY must be 0-100, got %d", cfg->ebpf.load_retry);
        errors++;
    }

    if (cfg->ebpf.load_delay < 0 || cfg->ebpf.load_delay > 3600) {
        LOG_ERROR("EBPF_LOAD_DELAY must be 0-3600 seconds, got %d", cfg->ebpf.load_delay);
        errors++;
    }

    return errors;
}

static int validate_service_params(atp_config_t *cfg) {
    int errors = 0;

    if (cfg->service.start_timeout_sec < 1 || cfg->service.start_timeout_sec > 3600) {
        LOG_ERROR("SERVICE_START_TIMEOUT must be 1-3600 seconds, got %d",
                  cfg->service.start_timeout_sec);
        errors++;
    }

    if (cfg->service.stop_timeout_sec < 1 || cfg->service.stop_timeout_sec > 3600) {
        LOG_ERROR("SERVICE_STOP_TIMEOUT must be 1-3600 seconds, got %d",
                  cfg->service.stop_timeout_sec);
        errors++;
    }

    if (cfg->service.grace_period_sec < 0 || cfg->service.grace_period_sec > 3600) {
        LOG_ERROR("SERVICE_GRACE_PERIOD must be 0-3600 seconds, got %d",
                  cfg->service.grace_period_sec);
        errors++;
    }

    if (cfg->service.max_failures < 1 || cfg->service.max_failures > 1000) {
        LOG_ERROR("SERVICE_MAX_FAILURES must be 1-1000, got %d", cfg->service.max_failures);
        errors++;
    }

    if (cfg->service.circuit_threshold < 1 || cfg->service.circuit_threshold > 1000) {
        LOG_ERROR("SERVICE_CIRCUIT_THRESHOLD must be 1-1000, got %d",
                  cfg->service.circuit_threshold);
        errors++;
    }

    if (cfg->service.circuit_cooldown_sec < 1 || cfg->service.circuit_cooldown_sec > 86400) {
        LOG_ERROR("SERVICE_CIRCUIT_COOLDOWN must be 1-86400 seconds, got %d",
                  cfg->service.circuit_cooldown_sec);
        errors++;
    }

    if (cfg->service.health_check_interval_ms < 100 ||
        cfg->service.health_check_interval_ms > 3600000) {
        LOG_ERROR("SERVICE_HEALTH_CHECK_INTERVAL must be 100-3600000 ms, got %d",
                  cfg->service.health_check_interval_ms);
        errors++;
    }

    return errors;
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

    errors += validate_dns_hijack_mode(cfg->network.dns_hijack) ? 1 : 0;

    if (cfg->filter.cnip_mode != 0 && cfg->filter.cnip_mode != 1) {
        LOG_ERROR("CNIP_MODE must be 0 (ipset) or 1 (ebpf), got %d", cfg->filter.cnip_mode);
        errors++;
    }

    if (cfg->filter.app_proxy_enable) {
        errors += validate_app_proxy_mode(cfg->filter.app_proxy_mode) ? 1 : 0;
    }
    if (cfg->filter.mac_filter_enable) {
        errors += validate_mac_proxy_mode(cfg->filter.mac_proxy_mode) ? 1 : 0;
    }

    errors += validate_clash_mode(cfg->filter.user_clash_mode) ? 1 : 0;

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

    errors += validate_bool(cfg->core.proxy_tcp, "PROXY_TCP") ? 1 : 0;
    errors += validate_bool(cfg->core.proxy_udp, "PROXY_UDP") ? 1 : 0;
    errors += validate_bool(cfg->network.proxy_ipv6, "PROXY_IPV6") ? 1 : 0;
    errors += validate_bool(cfg->filter.app_proxy_enable, "APP_PROXY_ENABLE") ? 1 : 0;
    errors += validate_bool(cfg->filter.mac_filter_enable, "MAC_FILTER_ENABLE") ? 1 : 0;
    errors += validate_bool(cfg->core.block_quic, "BLOCK_QUIC") ? 1 : 0;
    errors += validate_bool(cfg->core.log_timestamp, "LOG_TIMESTAMP") ? 1 : 0;
    errors += validate_bool(cfg->interface.proxy_mobile, "PROXY_MOBILE") ? 1 : 0;
    errors += validate_bool(cfg->interface.proxy_wifi, "PROXY_WIFI") ? 1 : 0;
    errors += validate_bool(cfg->interface.proxy_hotspot, "PROXY_HOTSPOT") ? 1 : 0;
    errors += validate_bool(cfg->interface.proxy_usb, "PROXY_USB") ? 1 : 0;
    errors += validate_bool(cfg->ebpf.enabled, "ENABLE_EBPF") ? 1 : 0;
    errors += validate_bool(cfg->core.ui_emoji_enabled, "UI_EMOJI_ENABLED") ? 1 : 0;

    errors += validate_ebpf_params(cfg);
    errors += validate_service_params(cfg);

    size_t cnip_len = strlen(cfg->filter.cnip_force_proxy_apps);
    if (cnip_len >= sizeof(cfg->filter.cnip_force_proxy_apps)) {
        LOG_ERROR("CNIP_FORCE_PROXY_APPS too long (max %zu)", sizeof(cfg->filter.cnip_force_proxy_apps) - 1);
        errors++;
    }

    if (errors > 0) {
        LOG_ERROR("Configuration validation found %d error(s)", errors);
        return -1;
    }

    return 0;
}
