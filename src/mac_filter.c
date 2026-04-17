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
    
    /* Format: XX:XX:XX:XX:XX:XX or XX-XX-XX-XX-XX-XX */
    int values[6];
    int count;
    
    if (strchr(mac_str, ':')) {
        count = sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    } else if (strchr(mac_str, '-')) {
        count = sscanf(mac_str, "%02x-%02x-%02x-%02x-%02x-%02x",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5]);
    } else {
        return -1;
    }
    
    if (count != 6) return -1;
    
    for (int i = 0; i < 6; i++) {
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
    /* Check if hotspot interface is set and different from WiFi */
    if (cfg->hotspot_iface[0] && strcmp(cfg->hotspot_iface, cfg->wifi_iface) != 0) {
        strncpy(iface, cfg->hotspot_iface, size - 1);
        iface[size - 1] = '\0';
        return 0;
    }
    return -1;
}

int mac_filter_setup(atp_config_t *cfg) {
    if (!cfg->mac_filter_enable) {
        LOG_DEBUG("MAC filter disabled");
        return 0;
    }
    
    /* Only apply MAC filter on hotspot interface */
    char hotspot_iface[IFNAMSIZ];
    if (mac_filter_get_hotspot_interface(cfg, hotspot_iface, sizeof(hotspot_iface)) < 0) {
        LOG_DEBUG("No dedicated hotspot interface, MAC filter skipped");
        return 0;
    }
    
    if (!cfg->proxy_hotspot) {
        LOG_DEBUG("Hotspot proxy disabled, MAC filter skipped");
        return 0;
    }
    
    LOG_INFO("Setting up MAC filter on %s (%s mode)", 
             hotspot_iface, cfg->mac_proxy_mode);
    
    mac_addr_t *macs;
    int mac_count;
    const char *macs_list = NULL;
    
    if (strcmp(cfg->mac_proxy_mode, "blacklist") == 0) {
        macs_list = cfg->bypass_macs_list;
        LOG_INFO("Blacklist mode: bypassing %d MAC addresses", mac_count);
    } else {
        macs_list = cfg->proxy_macs_list;
        LOG_INFO("Whitelist mode: proxying %d MAC addresses", mac_count);
    }
    
    if (mac_filter_parse_list(macs_list, &macs, &mac_count) < 0) {
        LOG_ERROR("Failed to parse MAC address list");
        return -1;
    }
    
    /* Flush existing MAC chain */
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_MAC_0");
    
    /* Add rules for each MAC address */
    for (int i = 0; i < mac_count; i++) {
        char rule[256];
        
        if (strcmp(cfg->mac_proxy_mode, "blacklist") == 0) {
            /* Blacklist: bypassed MACs go to ACCEPT */
            snprintf(rule, sizeof(rule), 
                     "-i %s -m mac --mac-source %s -j ACCEPT",
                     hotspot_iface, macs[i].addr_str);
            tproxy_rule_add(cfg, 4, "mangle", "ATP_MAC_0", rule);
        } else {
            /* Whitelist: proxied MACs go to RETURN (continue to TPROXY) */
            snprintf(rule, sizeof(rule), 
                     "-i %s -m mac --mac-source %s -j RETURN",
                     hotspot_iface, macs[i].addr_str);
            tproxy_rule_add(cfg, 4, "mangle", "ATP_MAC_0", rule);
        }
    }
    
    /* Add default rule */
    char rule[256];
    if (strcmp(cfg->mac_proxy_mode, "blacklist") == 0) {
        /* Blacklist: default is to proxy (RETURN -> TPROXY) */
        snprintf(rule, sizeof(rule), "-i %s -j RETURN", hotspot_iface);
    } else {
        /* Whitelist: default is to bypass (ACCEPT) */
        snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", hotspot_iface);
    }
    tproxy_rule_add(cfg, 4, "mangle", "ATP_MAC_0", rule);
    
    mac_filter_free_list(macs);
    return 0;
}

int mac_filter_cleanup(atp_config_t *cfg) {
    if (!cfg->mac_filter_enable) {
        return 0;
    }
    
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_MAC_0");
    LOG_INFO("MAC filter cleaned up");
    return 0;
}

int mac_filter_init(atp_config_t *cfg) {
    (void)cfg;
    LOG_DEBUG("MAC filter initialized");
    return 0;
}
