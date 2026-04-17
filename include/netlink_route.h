#ifndef ATP_NETLINK_ROUTE_H
#define ATP_NETLINK_ROUTE_H

#include <net/if.h>
#include <arpa/inet.h>

struct nl_route {
    int family;
    int table;
    int link_index;
    char iface[IFNAMSIZ];
    uint32_t priority;
    struct in6_addr dst;
    int dst_len;
    struct in6_addr src;
    int src_len;
    struct in6_addr gw;
    int protocol;
    int scope;
    int type;
    int flags;
};

int nl_route_list(struct nl_route **routes, int *count);
int nl_route_list_by_table(struct nl_route **routes, int *count, int table);
void nl_route_free(struct nl_route *routes, int count);
int nl_route_get_default_table(void);

#endif
