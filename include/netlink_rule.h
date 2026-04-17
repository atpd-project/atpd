#ifndef ATP_NETLINK_RULE_H
#define ATP_NETLINK_RULE_H

#include <stdint.h>
#include <stdbool.h>

#define NL_RULE_TABLE_UNSPEC  0
#define NL_RULE_PRIORITY_MAX  32768

struct nl_rule_uid_range {
    uint32_t start;
    uint32_t end;
};

struct nl_rule_port_range {
    uint16_t start;
    uint16_t end;
};

struct nl_rule {
    int family;           /* AF_INET or AF_INET6 */
    int table;            /* routing table ID */
    int priority;         /* rule priority */
    uint32_t mark;        /* firewall mark */
    uint32_t mark_mask;   /* mark mask */
    char iif_name[IFNAMSIZ];   /* incoming interface */
    char oif_name[IFNAMSIZ];   /* outgoing interface */
    struct nl_rule_uid_range *uid_range;
    struct nl_rule_port_range *sport_range;
    struct nl_rule_port_range *dport_range;
    int ip_proto;         /* IP protocol */
    int action;           /* FR_ACT_* */
    int flags;            /* FIB_RULE_* */
    int goto_target;      /* for FR_ACT_GOTO */
};

int nl_rule_list(struct nl_rule **rules, int *count);
void nl_rule_free(struct nl_rule *rules, int count);
int nl_vpn_detect(void);
int nl_rule_add(int family, int table, int priority, uint32_t mark, const char *iif_name);
int nl_rule_del(int family, int table, int priority, uint32_t mark, const char *iif_name);

#endif
