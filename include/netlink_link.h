#ifndef ATP_NETLINK_LINK_H
#define ATP_NETLINK_LINK_H

#include <stdint.h>
#include <net/if.h>
#include <arpa/inet.h>

#define NL_LINK_FLAG_UP         0x1
#define NL_LINK_FLAG_RUNNING    0x2
#define NL_LINK_FLAG_LOOPBACK   0x4
#define NL_LINK_FLAG_POINTOPOINT 0x8

struct nl_link_addr {
    uint8_t data[8];
    int len;
};

struct nl_link {
    int index;                    /* interface index */
    char name[IFNAMSIZ];          /* interface name */
    char qdisc[IFNAMSIZ];         /* queueing discipline */
    int flags;                    /* interface flags (IFF_*) */
    int mtu;                      /* MTU value */
    int tx_queue_len;             /* TX queue length */
    struct nl_link_addr address;  /* hardware address */
    struct nl_link_addr broadcast;/* broadcast address */
    int carrier;                  /* carrier state (0/1) */
    int carrier_changes;          /* carrier change count */
};

int nl_link_list(struct nl_link **links, int *count);
void nl_link_free(struct nl_link *links, int count);
int nl_link_get_by_name(const char *name, struct nl_link *link);
int nl_link_get_by_index(int index, struct nl_link *link);
int nl_link_get_index_by_name(const char *name);
const char* nl_link_get_name_by_index(int index);
int nl_link_get_vpn_interface(char *iface, size_t size);

#endif
