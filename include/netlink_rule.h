#ifndef ATP_NETLINK_RULE_H
#define ATP_NETLINK_RULE_H

#include <stdint.h>
#include <netinet/in.h>

struct nl_rule_uid_range {
    uint32_t start;
    uint32_t end;
};

struct nl_rule {
    int family;
    int table;
    int priority;
    uint32_t mark;
    uint32_t mark_mask;
    char iif_name[IFNAMSIZ];
    char oif_name[IFNAMSIZ];
    struct nl_rule_uid_range *uid_range;
    int action;
    int flags;
};

int nl_rule_list(struct nl_rule **rules, int *count);
void nl_rule_free(struct nl_rule *rules, int count);
int nl_vpn_detect(void);

#endif
