/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * MAC address filter implementation
 */

#include "mac_filter.h"
#include "logger.h"
#include "utils.h"
#include "tproxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <net/if.h>
#include <net/ethernet.h>

int mac_filter_parse_mac(const char *mac_str, uint8_t *mac_bytes) {
    if (!mac_str || !mac_bytes) return -1;
    
    unsigned int values[6];
    int count;
    
    if (strchr(mac_str, ':')) {
        count = sscanf(mac_str, "%2x:%2x:%2x:%2x:%2x:%2x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    } else if (strchr(mac_str, '-')) {
        count = sscanf(mac_str, "%2x-%2x-%2x-%2x-%2x-%2x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    } else {
        return -1;
    }
    
    if (count != 6) return -1;
    
    for (int i = 0; i < 6; i++) {
        if (values[i] > 0xFF) return -1;
        mac_bytes[i] = (uint8_t)values[i];
    }
    
    return 0;
}

void mac_filter_format_mac(const uint8_t *mac_bytes, char *buf, size_t size) {
    if (!mac_bytes || !buf) return;
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_bytes[0], mac_bytes[1], mac_bytes[2],
             mac_bytes[3], mac_bytes[4], mac_bytes[5]);
}

int mac_filter_parse_list(const char *macs_list, mac_addr_t **macs, int *count) {
    if (!macs_list || macs_list[0] == '\0') {
        *macs = NULL;
        *count = 0;
        return 0;
    }
    
    char *str_copy = strdup(macs_list);
    if (!str_copy) return -1;
    
    mac_addr_t *mac_array = NULL;
    int mac_count = 0;
    char *token = strtok(str_copy, " ,");
    
    while (token) {
        uint8_t mac_bytes[ETH_ALEN];
        if (mac_filter_parse_mac(token, mac_bytes) == 0) {
            mac_addr_t *new_array = realloc(mac_array, sizeof(mac_addr_t) * (mac_count + 1));
            if (!new_array) {
                for (int i = 0; i < mac_count; i++) free(mac_array);
                free(str_copy);
                return -1;
            }
            mac_array = new_array;
            memcpy(mac_array[mac_count].addr, mac_bytes, ETH_ALEN);
            mac_filter_format_mac(mac_bytes, mac_array[mac_count].addr_str, sizeof(mac_array[mac_count].addr_str));
            mac_count++;
        } else {
            LOG_WARN("Invalid MAC address format: %s", token);
        }
        token = strtok(NULL, " ,");
    }
    
    free(str_copy);
    *macs = mac_array;
    *count = mac_count;
    return 0;
}

void mac_filter_free_list(mac_addr_t *macs) {
    if (macs) free(macs);
}

static int mac_filter_get_hotspot_interface(atp_config_t *cfg, char *iface, size_t size) {
    if (cfg->interface.hotspot_iface[0] && strcmp(cfg->interface.hotspot_iface, cfg->interface.wifi_iface) != 0) {
        strncpy(iface, cfg->interface.hotspot_iface, size - 1);
        iface[size - 1] = '\0';
        return 0;
    }
    return -1;
}

static int mac_filter_configure_chain(atp_config_t *cfg, int family, 
                                        const char *chain_name, const char *hotspot_iface,
                                        mac_addr_t *macs, int mac_count) {
    const char *table = "mangle";
    
    tproxy_chain_flush(cfg, family, table, chain_name);
    
    for (int i = 0; i < mac_count; i++) {
        char rule[256];
        
        if (strcmp(cfg->filter.mac_proxy_mode, "blacklist") == 0) {
            snprintf(rule, sizeof(rule), 
                     "-i %s -m mac --mac-source %s -j ACCEPT",
                     hotspot_iface, macs[i].addr_str);
        } else {
            snprintf(rule, sizeof(rule), 
                     "-i %s -m mac --mac-source %s -j RETURN",
                     hotspot_iface, macs[i].addr_str);
        }
        
        if (tproxy_rule_add(cfg, family, table, chain_name, rule) != 0) {
            LOG_ERROR("MAC filter: failed to add rule for %s on %s", macs[i].addr_str, chain_name);
        }
    }
    
    char rule[256];
    if (strcmp(cfg->filter.mac_proxy_mode, "blacklist") == 0) {
        snprintf(rule, sizeof(rule), "-i %s -j RETURN", hotspot_iface);
    } else {
        snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", hotspot_iface);
    }
    
    if (tproxy_rule_add(cfg, family, table, chain_name, rule) != 0) {
        LOG_ERROR("MAC filter: failed to add default rule on %s", chain_name);
    }
    
    return 0;
}

int mac_filter_setup(atp_config_t *cfg) {
    if (!cfg->filter.mac_filter_enable) {
        LOG_DEBUG("MAC filter disabled");
        return 0;
    }
    
    char hotspot_iface[IFNAMSIZ];
    if (mac_filter_get_hotspot_interface(cfg, hotspot_iface, sizeof(hotspot_iface)) < 0) {
        LOG_DEBUG("No dedicated hotspot interface, MAC filter skipped");
        return 0;
    }
    
    if (!cfg->interface.proxy_hotspot) {
        LOG_DEBUG("Hotspot proxy disabled, MAC filter skipped");
        return 0;
    }
    
    mac_addr_t *macs;
    int mac_count;
    const char *macs_list = NULL;
    
    if (strcmp(cfg->filter.mac_proxy_mode, "blacklist") == 0) {
        macs_list = cfg->filter.bypass_macs_list;
    } else {
        macs_list = cfg->filter.proxy_macs_list;
    }
    
    if (mac_filter_parse_list(macs_list, &macs, &mac_count) < 0) {
        LOG_ERROR("Failed to parse MAC address list");
        return -1;
    }
    
    if (strcmp(cfg->filter.mac_proxy_mode, "blacklist") == 0) {
        LOG_INFO("Blacklist mode: bypassing %d MAC addresses", mac_count);
    } else {
        LOG_INFO("Whitelist mode: proxying %d MAC addresses", mac_count);
    }
    
    mac_filter_configure_chain(cfg, 4, "ATP_MAC_0", hotspot_iface, macs, mac_count);
    
    if (cfg->network.proxy_ipv6) {
        mac_filter_configure_chain(cfg, 6, "ATP6_MAC_0", hotspot_iface, macs, mac_count);
    }
    
    LOG_INFO("MAC filter configured on %s (IPv6: %s)", 
             hotspot_iface, cfg->network.proxy_ipv6 ? "enabled" : "disabled");
    
    mac_filter_free_list(macs);
    return 0;
}

int mac_filter_cleanup(atp_config_t *cfg) {
    if (!cfg->filter.mac_filter_enable) {
        return 0;
    }
    
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_MAC_0");
    
    if (cfg->network.proxy_ipv6) {
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_MAC_0");
    }
    
    LOG_INFO("MAC filter cleaned up");
    return 0;
}

int mac_filter_init(atp_config_t *cfg) {
    (void)cfg;
    LOG_DEBUG("MAC filter initialized");
    return 0;
}
