#ifndef ATP_NETLINK_H
#define ATP_NETLINK_H

#include "atp.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

typedef struct {
    char iface[IFNAMSIZ];
    int ifindex;
    int has_ipv4;
    char ipv4_addr[INET_ADDRSTRLEN];
    int ipv4_prefix;
    int has_ipv6;
    char ipv6_addr[INET6_ADDRSTRLEN];
    int ipv6_prefix;
    unsigned int flags;
    unsigned int mtu;
} netlink_iface_info_t;

typedef void (*netlink_callback_t)(const char *iface, int added, int ifindex, void *userdata);

typedef struct {
    int sock_fd;
    int epoll_fd;
    volatile int running;
    netlink_callback_t callback;
    void *callback_data;
} netlink_ctx_t;

int netlink_init(netlink_ctx_t *ctx);
void netlink_cleanup(netlink_ctx_t *ctx);
int netlink_monitor_start(netlink_ctx_t *ctx, netlink_callback_t callback, void *userdata);
int netlink_monitor_stop(netlink_ctx_t *ctx);
int netlink_get_active_vpn(netlink_ctx_t *ctx, char *iface, size_t size);
int netlink_wait_for_iface(const char *iface, int timeout_sec);
int netlink_get_iface_info(const char *iface, netlink_iface_info_t *info);
int netlink_get_all_ifaces(netlink_iface_info_t *info_array, int max_count);
int netlink_check_rule_exists(int table_id, int mark, const char *iface);
int netlink_get_ipv4_snapshot(char *output, size_t size);
int netlink_compare_ipv4_snapshot(const char *before, const char *after, char *diff, size_t size);

#endif