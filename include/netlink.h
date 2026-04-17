#ifndef ATP_NETLINK_H
#define ATP_NETLINK_H

#include <stddef.h>
#include <stdint.h>
#include <net/if.h>

/* Include netlink submodules */
#include "netlink_rule.h"
#include "netlink_route.h"
#include "netlink_link.h"

/* Unified netlink initialization and cleanup */
int nl_init(void);
void nl_cleanup(void);

/* Interface monitoring and waiting */
int netlink_wait_for_iface(const char *iface, int timeout_sec);
int netlink_get_iface_info(const char *iface, void *info);

/* Legacy compatibility functions (for existing code) */
int netlink_get_active_vpn(char *iface, size_t size);
int netlink_get_ipv4_snapshot(char *output, size_t size);
int netlink_check_rule_exists(int table_id, int mark, const char *iface);

#endif
