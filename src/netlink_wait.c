/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Netlink wait utilities
 */

#include "netlink.h"
#include "logger.h"
#include <unistd.h>
#include <string.h>

int netlink_wait_for_iface(const char *iface, int timeout_sec) {
    int waited = 0;
    
    if (!iface || !iface[0]) {
        LOG_ERROR("netlink_wait_for_iface: invalid interface name");
        return -1;
    }
    
    LOG_DEBUG("Waiting for interface %s (timeout: %d seconds)", iface, timeout_sec);
    
    while (waited < timeout_sec) {
        char active_vpn[IFNAMSIZ];
        
        if (netlink_get_active_vpn(active_vpn, sizeof(active_vpn)) == 0) {
            if (strcmp(active_vpn, iface) == 0) {
                LOG_INFO("Interface %s is ready after %d seconds", iface, waited);
                return 0;
            }
        }
        
        sleep(1);
        waited++;
    }
    
    LOG_WARN("Interface %s not ready after %d seconds", iface, timeout_sec);
    return -1;
}
