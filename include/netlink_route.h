#ifndef ATP_NETLINK_ROUTE_H
#define ATP_NETLINK_ROUTE_H

#include <stdint.h>
#include <net/if.h>
#include <arpa/inet.h>

#define NL_ROUTE_TABLE_MAIN   254
#define NL_ROUTE_TABLE_LOCAL  255

struct nl_route {
    int family;           /* AF_INET or AF_INET6 */
    int table;            /* routing table ID */
    int link_index;       /* output interface index */
    char iface[IFNAMSIZ]; /* output interface name */
    uint32_t priority;    /* route priority (metric) */
    uint32_t mark;        /* firewall mark */
    struct in6_addr dst;  /* destination network */
    int dst_len;          /* destination prefix length */
    struct in6_addr src;  /* source address */
    int src_len;          /* source prefix length */
    struct in6_addr gw;   /* gateway address */
    int protocol;         /* route protocol (RTPROT_*) */
    int scope;            /* route scope (RT_SCOPE_*) */
    int type;             /* route type (RTN_*) */
    int flags;            /* route flags (RTM_F_*) */
};

int nl_route_list(struct nl_route **routes, int *count);
int nl_route_list_by_table(struct nl_route **routes, int *count, int table);
void nl_route_free(struct nl_route *routes, int count);
int nl_route_get_default_table(void);
int nl_route_get_table_by_mark(uint32_t mark);

#endif
