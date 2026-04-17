#ifndef ATP_NETLINK_LINK_H
#define ATP_NETLINK_LINK_H

#include <net/if.h>
#include <stdint.h>

struct nl_link_addr {
    uint8_t data[8];
    int len;
};

struct nl_link {
    int index;
    char name[IFNAMSIZ];
    int flags;
    int mtu;
    struct nl_link_addr address;
};

int nl_link_list(struct nl_link **links, int *count);
void nl_link_free(struct nl_link *links, int count);
int nl_link_get_by_name(const char *name, struct nl_link *link);
int nl_link_get_vpn_interface(char *iface, size_t size);

#endif
