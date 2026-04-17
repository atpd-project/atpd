#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>

#define IP_CMD "/system/bin/ip"

/* Forward declarations for legacy functions */
static int nl_get_active_vpn_legacy(char *iface, size_t size);
static int nl_get_ipv4_snapshot_legacy(char *output, size_t size);
static int nl_check_rule_exists_legacy(int table_id, int mark, const char *iface);

int nl_init(void) {
    LOG_DEBUG("Netlink subsystem initialized");
    return 0;
}

void nl_cleanup(void) {
    LOG_DEBUG("Netlink subsystem cleaned up");
}

/* Legacy compatibility: use netlink if available, fallback to ip command */
int netlink_get_active_vpn(char *iface, size_t size) {
    /* First try netlink-based detection */
    if (nl_link_get_vpn_interface(iface, size) == 0) {
        return 0;
    }
    
    /* Fallback to legacy method */
    return nl_get_active_vpn_legacy(iface, size);
}

int netlink_get_ipv4_snapshot(char *output, size_t size) {
    /* Use netlink to get IPv4 addresses */
    struct nl_link *links;
    int count;
    char buf[4096] = {0};
    char *ptr = buf;
    
    if (nl_link_list(&links, &count) < 0) {
        return nl_get_ipv4_snapshot_legacy(output, size);
    }
    
    for (int i = 0; i < count; i++) {
        /* Skip loopback interface */
        if (links[i].flags & IFF_LOOPBACK) continue;
        if (!(links[i].flags & IFF_RUNNING)) continue;
        
        /* Get IPv4 address for this interface (simplified) */
        char cmd[256];
        char addr[64];
        snprintf(cmd, sizeof(cmd), 
                 "%s -4 addr show dev %s 2>/dev/null | grep 'inet ' | head -1 | awk '{print $2}'",
                 IP_CMD, links[i].name);
        
        if (exec_cmd(cmd, addr, sizeof(addr), 3) == 0 && addr[0] != '\0') {
            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), 
                            "%s:%s ", links[i].name, addr);
        }
    }
    
    nl_link_free(links, count);
    
    if (ptr > buf) {
        strncpy(output, buf, size - 1);
        output[size - 1] = '\0';
        return 0;
    }
    
    return nl_get_ipv4_snapshot_legacy(output, size);
}

int netlink_check_rule_exists(int table_id, int mark, const char *iface) {
    /* Use netlink to check rule existence */
    struct nl_rule *rules;
    int count;
    
    if (nl_rule_list(&rules, &count) < 0) {
        return nl_check_rule_exists_legacy(table_id, mark, iface);
    }
    
    int exists = 0;
    for (int i = 0; i < count; i++) {
        if (rules[i].table == table_id && rules[i].mark == (uint32_t)mark) {
            exists = 1;
            break;
        }
        if (iface && iface[0] && strcmp(rules[i].iif_name, iface) == 0) {
            exists = 1;
            break;
        }
    }
    
    nl_rule_free(rules, count);
    return exists;
}

/* Legacy fallback functions using ip command */
static int nl_get_active_vpn_legacy(char *iface, size_t size) {
    char cmd[256];
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

static int nl_get_ipv4_snapshot_legacy(char *output, size_t size) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), 
             "%s -4 addr show | grep 'inet ' | awk '{print $NF\":\"$2}' | grep -v lo: | tr '\\n' ' '",
             IP_CMD);
    return exec_cmd(cmd, output, size, 5);
}

static int nl_check_rule_exists_legacy(int table_id, int mark, const char *iface) {
    char cmd[256];
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
