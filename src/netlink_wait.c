#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define IP_CMD "/system/bin/ip"

int netlink_wait_for_iface(const char *iface, int timeout_sec) {
    char cmd[MAX_CMD_LEN];
    int waited = 0;
    
    if (!iface || !iface[0]) {
        LOG_ERROR("netlink_wait_for_iface: invalid interface name");
        return -1;
    }
    
    LOG_DEBUG("Waiting for interface %s (timeout: %d seconds)", iface, timeout_sec);
    
    while (waited < timeout_sec) {
        /* Check if interface exists and has IPv4 address */
        snprintf(cmd, sizeof(cmd), 
                 "%s -4 addr show dev %s 2>/dev/null | grep -q 'inet '", 
                 IP_CMD, iface);
        
        if (exec_cmd_simple(cmd, 2) == 0) {
            LOG_INFO("Interface %s is ready after %d seconds", iface, waited);
            return 0;
        }
        
        /* Also check if interface simply exists (may not have IP yet) */
        snprintf(cmd, sizeof(cmd), 
                 "%s link show dev %s 2>/dev/null | grep -q 'UP'", 
                 IP_CMD, iface);
        
        if (exec_cmd_simple(cmd, 2) == 0 && waited >= 2) {
            LOG_INFO("Interface %s is up (waiting for IP address)", iface);
        }
        
        sleep(1);
        waited++;
    }
    
    LOG_WARN("Interface %s not ready after %d seconds", iface, timeout_sec);
    return -1;
}

int netlink_get_iface_info(const char *iface, void *info_ptr) {
    /* This function is a compatibility wrapper */
    /* The actual implementation is in netlink_link.c */
    
    if (!iface || !iface[0] || !info_ptr) {
        return -1;
    }
    
    /* Use netlink_link.h functions */
    return nl_link_get_by_name(iface, (struct nl_link*)info_ptr);
}
