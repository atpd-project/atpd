#ifndef ATP_NETLINK_H
#define ATP_NETLINK_H

#include <stddef.h>
#include <net/if.h>

/* Legacy compatibility functions */
int netlink_get_active_vpn(char *iface, size_t size);
int netlink_get_ipv4_snapshot(char *output, size_t size);
int netlink_check_rule_exists(int table_id, int mark, const char *iface);
int netlink_wait_for_iface(const char *iface, int timeout_sec);
int netlink_get_iface_info(const char *iface, void *info);

/* VPN detection functions for iface_monitor.c */
int nl_vpn_detect(void);
int nl_link_get_vpn_interface(char *iface, size_t size);

int netlink_init(void);
void netlink_cleanup(void);
#endif
