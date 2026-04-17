#include "netlink_rule.h"
#include "logger.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/fib_rules.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define NL_BUF_SIZE 8192
#define NL_SEQ 12345

static int nl_send_request(int sock, int family, int type, int flags) {
    struct {
        struct nlmsghdr nlh;
        struct rtgenmsg g;
    } req;
    
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    req.nlh.nlmsg_type = type;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | flags;
    req.nlh.nlmsg_seq = NL_SEQ;
    req.nlh.nlmsg_pid = getpid();
    req.g.rtgen_family = family;
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    struct iovec iov = { &req, req.nlh.nlmsg_len };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    return sendmsg(sock, &msg, 0);
}

static int nl_recv_response(int sock, void **data, int *len) {
    char buf[NL_BUF_SIZE];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    ssize_t recv_len = recvmsg(sock, &msg, 0);
    if (recv_len < 0) return -1;
    
    *data = malloc(recv_len);
    if (!*data) return -1;
    memcpy(*data, buf, recv_len);
    *len = recv_len;
    return 0;
}

static int nl_parse_rule_attrs(struct nl_rule *rule, struct rtattr *attrs, int len) {
    struct rtattr *rta;
    int rta_len = len;
    
    for (rta = attrs; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case FRA_TABLE:
                rule->table = *((uint32_t*)RTA_DATA(rta));
                break;
            case FRA_PRIORITY:
                rule->priority = *((uint32_t*)RTA_DATA(rta));
                break;
            case FRA_FWMARK:
                rule->mark = *((uint32_t*)RTA_DATA(rta));
                break;
            case FRA_FWMASK:
                rule->mark_mask = *((uint32_t*)RTA_DATA(rta));
                break;
            case FRA_IIFNAME:
                strncpy(rule->iif_name, (char*)RTA_DATA(rta), IFNAMSIZ - 1);
                rule->iif_name[IFNAMSIZ - 1] = '\0';
                break;
            case FRA_OIFNAME:
                strncpy(rule->oif_name, (char*)RTA_DATA(rta), IFNAMSIZ - 1);
                rule->oif_name[IFNAMSIZ - 1] = '\0';
                break;
            case FRA_UID_RANGE: {
                struct fib_rule_uid_range *ur = (struct fib_rule_uid_range*)RTA_DATA(rta);
                rule->uid_range = malloc(sizeof(struct nl_rule_uid_range));
                if (rule->uid_range) {
                    rule->uid_range->start = ur->start;
                    rule->uid_range->end = ur->end;
                }
                break;
            }
            case FRA_SPORT_RANGE: {
                struct fib_rule_port_range *pr = (struct fib_rule_port_range*)RTA_DATA(rta);
                rule->sport_range = malloc(sizeof(struct nl_rule_port_range));
                if (rule->sport_range) {
                    rule->sport_range->start = pr->start;
                    rule->sport_range->end = pr->end;
                }
                break;
            }
            case FRA_DPORT_RANGE: {
                struct fib_rule_port_range *pr = (struct fib_rule_port_range*)RTA_DATA(rta);
                rule->dport_range = malloc(sizeof(struct nl_rule_port_range));
                if (rule->dport_range) {
                    rule->dport_range->start = pr->start;
                    rule->dport_range->end = pr->end;
                }
                break;
            }
            case FRA_IP_PROTO:
                rule->ip_proto = *((uint8_t*)RTA_DATA(rta));
                break;
            case FRA_GOTO:
                rule->goto_target = *((uint32_t*)RTA_DATA(rta));
                break;
        }
    }
    
    return 0;
}

int nl_rule_list(struct nl_rule **rules, int *count) {
    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0) {
        LOG_ERROR("Failed to create netlink socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind netlink socket: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    if (nl_send_request(sock, AF_UNSPEC, RTM_GETRULE, NLM_F_DUMP) < 0) {
        LOG_ERROR("Failed to send netlink request: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    void *data;
    int data_len;
    if (nl_recv_response(sock, &data, &data_len) < 0) {
        LOG_ERROR("Failed to receive netlink response: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    close(sock);
    
    struct nl_rule *rule_list = NULL;
    int rule_count = 0;
    struct nlmsghdr *nlh;
    
    for (nlh = (struct nlmsghdr*)data; NLMSG_OK(nlh, data_len); nlh = NLMSG_NEXT(nlh, data_len)) {
        if (nlh->nlmsg_type == NLMSG_DONE) break;
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            LOG_ERROR("Netlink error response");
            free(data);
            return -1;
        }
        
        if (nlh->nlmsg_type == RTM_NEWRULE) {
            struct fib_rule_hdr *frh = (struct fib_rule_hdr*)NLMSG_DATA(nlh);
            int attr_len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*frh));
            
            struct nl_rule *rule = malloc(sizeof(struct nl_rule));
            if (!rule) continue;
            
            memset(rule, 0, sizeof(struct nl_rule));
            rule->family = frh->family;
            rule->action = frh->action;
            rule->flags = frh->flags;
            
            if (frh->table != NL_RULE_TABLE_UNSPEC) {
                rule->table = frh->table;
            }
            
            if (attr_len > 0) {
                struct rtattr *rta = (struct rtattr*)((char*)frh + NLMSG_ALIGN(sizeof(*frh)));
                nl_parse_rule_attrs(rule, rta, attr_len);
            }
            
            rule_list = realloc(rule_list, sizeof(struct nl_rule) * (rule_count + 1));
            if (!rule_list) {
                free(rule);
                break;
            }
            rule_list[rule_count++] = *rule;
            free(rule);
        }
    }
    
    free(data);
    *rules = rule_list;
    *count = rule_count;
    return 0;
}

void nl_rule_free(struct nl_rule *rules, int count) {
    if (!rules) return;
    
    for (int i = 0; i < count; i++) {
        if (rules[i].uid_range) free(rules[i].uid_range);
        if (rules[i].sport_range) free(rules[i].sport_range);
        if (rules[i].dport_range) free(rules[i].dport_range);
    }
    free(rules);
}

int nl_vpn_detect(void) {
    struct nl_rule *rules;
    int count;
    
    if (nl_rule_list(&rules, &count) < 0) {
        return 0;
    }
    
    int vpn_enabled = 0;
    for (int i = 0; i < count; i++) {
        /* Android VPN rule has FRA_UID_RANGE attribute */
        if (rules[i].uid_range != NULL) {
            vpn_enabled = 1;
            break;
        }
    }
    
    nl_rule_free(rules, count);
    return vpn_enabled;
}

int nl_rule_add(int family, int table, int priority, uint32_t mark, const char *iif_name) {
    /* TODO: Implement rule addition via netlink */
    LOG_WARN("nl_rule_add not yet implemented, use ip command instead");
    return -1;
}

int nl_rule_del(int family, int table, int priority, uint32_t mark, const char *iif_name) {
    /* TODO: Implement rule deletion via netlink */
    LOG_WARN("nl_rule_del not yet implemented, use ip command instead");
    return -1;
}
