#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>

#define IP_CMD "/system/bin/ip"

int netlink_get_active_vpn(char *iface, size_t size) {
    char cmd[MAX_CMD_LEN];
    char output[64];
    
    snprintf(cmd, sizeof(cmd), 
             "%s -4 addr | grep -oE 'ipsec[0-9]+|tun[0-9]+|wg[0-9]+' | head -1",
             IP_CMD);
    
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        strncpy(iface, output, size - 1);
        iface[size - 1] = '\0';
        return 0;
    }
    
    iface[0] = '\0';
    return -1;
}

int netlink_get_ipv4_snapshot(char *output, size_t size) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "%s -4 addr show | grep 'inet ' | awk '{print $NF\":\"$2}' | grep -v lo: | tr '\\n' ' '",
             IP_CMD);
    return exec_cmd(cmd, output, size, 5);
}

int netlink_check_rule_exists(int table_id, int mark, const char *iface) {
    char cmd[MAX_CMD_LEN];
    char output[64];
    
    snprintf(cmd, sizeof(cmd), 
             "%s rule show | grep -q 'fwmark 0x%x.*lookup %d'",
             IP_CMD, mark, table_id);
    
    if (exec_cmd(cmd, output, sizeof(output), 5) != 0) {
        return 0;
    }
    
    if (iface && iface[0]) {
        snprintf(cmd, sizeof(cmd), 
                 "%s rule show | grep -q 'iif %s'", IP_CMD, iface);
        return exec_cmd(cmd, output, sizeof(output), 5) == 0;
    }
    
    return 1;
}

int netlink_wait_for_iface(const char *iface, int timeout_sec) {
    char cmd[MAX_CMD_LEN];
    int waited = 0;
    
    if (!iface || !iface[0]) {
        LOG_ERROR("netlink_wait_for_iface: invalid interface name");
        return -1;
    }
    
    LOG_DEBUG("Waiting for interface %s (timeout: %d seconds)", iface, timeout_sec);
    
    while (waited < timeout_sec) {
        snprintf(cmd, sizeof(cmd), 
                 "%s -4 addr show dev %s 2>/dev/null | grep -q 'inet '", 
                 IP_CMD, iface);
        
        if (exec_cmd_simple(cmd, 2) == 0) {
            LOG_INFO("Interface %s is ready after %d seconds", iface, waited);
            return 0;
        }
        
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
    (void)iface;
    (void)info_ptr;
    return -1;
}

/* VPN detection functions for iface_monitor.c */

int nl_vpn_detect(void) {
    char vpn_iface[IFNAMSIZ];
    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0] != '\0') {
        LOG_DEBUG("VPN detected on interface: %s", vpn_iface);
        return 1;
    }
    return 0;
}

int nl_link_get_vpn_interface(char *iface, size_t size) {
    return netlink_get_active_vpn(iface, size);
}

int netlink_init(void) {
    /* Netlink is initialized on-demand, nothing to do here */
    return 0;
}

void netlink_cleanup(void) {
    /* Nothing to clean up */
}
