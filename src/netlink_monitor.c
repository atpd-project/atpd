/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Netlink - Pure native Netlink state machine
 * Zero exec_cmd, zero iproute2, zero /proc parsing
 */

#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/fib_rules.h>
#include <pthread.h>

#define NL_BUFFER_SIZE 65536

static int g_sync_fd = -1;
static int g_async_fd = -1;
static nl_callback_t g_callback = NULL;
static void *g_userdata = NULL;
static uint32_t g_seq = 1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

struct nl_link_info {
    int index;
    char name[IFNAMSIZ];
    unsigned int flags;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
};

struct nl_addr_info {
    int ifindex;
    int family;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } addr;
    int prefix_len;
};

struct nl_route_info {
    int family;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } dst;
    int dst_len;
    int ifindex;
};

struct nl_rule_info {
    int family;
    uint32_t table;
    uint32_t mark;
    uint32_t mask;
    int priority;
};

struct nl_parse_ctx {
    union {
        struct nl_link_info *links;
        struct nl_addr_info *addrs;
        struct nl_route_info *routes;
        struct nl_rule_info *rules;
    } data;
    int max_count;
    int count;
    int error;
};

static int is_vpn_interface(const char *iface) {
    if (!iface) return 0;
    return (strncmp(iface, "ipsec", 5) == 0 ||
            strncmp(iface, "tun", 3) == 0 ||
            strncmp(iface, "wg", 2) == 0 ||
            strncmp(iface, "vpn", 3) == 0);
}

static int netlink_recv_multi(int fd, uint32_t seq, 
                               int (*parser)(struct nlmsghdr *, void *), 
                               void *ctx) {
    char buf[NL_BUFFER_SIZE];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    int multi = 0;
    
    while (1) {
        struct msghdr msg = {
            .msg_name = &addr,
            .msg_namelen = sizeof(addr),
            .msg_iov = &iov,
            .msg_iovlen = 1
        };
        
        ssize_t len = recvmsg(fd, &msg, 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Netlink: recvmsg failed: %s", strerror(errno));
            return -1;
        }
        
        if (len == 0) break;
        
        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
        
        for (; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_seq != seq) continue;
            
            if (nlh->nlmsg_type == NLMSG_DONE) {
                return 0;
            }
            
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(nlh);
                if (err->error) {
                    LOG_ERROR("Netlink error: %s", strerror(-err->error));
                    return err->error;
                }
                return 0;
            }
            
            if (nlh->nlmsg_flags & NLM_F_MULTI) {
                multi = 1;
            }
            
            if (parser && parser(nlh, ctx) != 0) {
                return 0;
            }
        }
        
        if (!multi) break;
    }
    
    return 0;
}

static int parse_link(struct nlmsghdr *nlh, void *ctx) {
    struct nl_parse_ctx *pctx = (struct nl_parse_ctx *)ctx;
    struct ifinfomsg *ifi = NLMSG_DATA(nlh);
    
    if (pctx->count >= pctx->max_count) {
        pctx->error = -1;
        return -1;
    }
    
    struct nl_link_info *info = &pctx->data.links[pctx->count];
    info->index = ifi->ifi_index;
    info->flags = ifi->ifi_flags;
    info->rx_bytes = 0;
    info->tx_bytes = 0;
    info->name[0] = '\0';
    
    struct rtattr *rta = IFLA_RTA(ifi);
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifi));
    
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case IFLA_IFNAME:
                strncpy(info->name, RTA_DATA(rta), IFNAMSIZ - 1);
                break;
            case IFLA_STATS64: {
                struct rtnl_link_stats64 *stats = RTA_DATA(rta);
                info->rx_bytes = stats->rx_bytes;
                info->tx_bytes = stats->tx_bytes;
                break;
            }
        }
    }
    
    if (info->name[0]) pctx->count++;
    return 0;
}

static int parse_addr(struct nlmsghdr *nlh, void *ctx) {
    struct nl_parse_ctx *pctx = (struct nl_parse_ctx *)ctx;
    struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
    
    if (pctx->count >= pctx->max_count) {
        pctx->error = -1;
        return -1;
    }
    
    struct nl_addr_info *info = &pctx->data.addrs[pctx->count];
    info->ifindex = ifa->ifa_index;
    info->family = ifa->ifa_family;
    info->prefix_len = ifa->ifa_prefixlen;
    memset(&info->addr, 0, sizeof(info->addr));
    
    struct rtattr *rta = IFA_RTA(ifa);
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifa));
    
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFA_ADDRESS) {
            if (ifa->ifa_family == AF_INET) {
                memcpy(&info->addr.v4, RTA_DATA(rta), 4);
            } else if (ifa->ifa_family == AF_INET6) {
                memcpy(&info->addr.v6, RTA_DATA(rta), 16);
            }
            pctx->count++;
            break;
        }
    }
    
    return 0;
}

static int parse_route(struct nlmsghdr *nlh, void *ctx) {
    struct nl_parse_ctx *pctx = (struct nl_parse_ctx *)ctx;
    struct rtmsg *rtm = NLMSG_DATA(nlh);
    
    if (rtm->rtm_table != RT_TABLE_MAIN) return 0;
    if (pctx->count >= pctx->max_count) {
        pctx->error = -1;
        return -1;
    }
    
    struct nl_route_info *info = &pctx->data.routes[pctx->count];
    info->family = rtm->rtm_family;
    info->dst_len = rtm->rtm_dst_len;
    info->ifindex = 0;
    memset(&info->dst, 0, sizeof(info->dst));
    
    struct rtattr *rta = RTM_RTA(rtm);
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*rtm));
    
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case RTA_DST:
                if (rtm->rtm_family == AF_INET) {
                    memcpy(&info->dst.v4, RTA_DATA(rta), 4);
                } else if (rtm->rtm_family == AF_INET6) {
                    memcpy(&info->dst.v6, RTA_DATA(rta), 16);
                }
                break;
            case RTA_OIF:
                info->ifindex = *(int *)RTA_DATA(rta);
                break;
        }
    }
    
    pctx->count++;
    return 0;
}

static int parse_rule(struct nlmsghdr *nlh, void *ctx) {
    struct nl_parse_ctx *pctx = (struct nl_parse_ctx *)ctx;
    struct fib_rule_hdr *frh = NLMSG_DATA(nlh);
    
    if (pctx->count >= pctx->max_count) {
        pctx->error = -1;
        return -1;
    }
    
    struct nl_rule_info *info = &pctx->data.rules[pctx->count];
    info->family = frh->family;
    info->table = frh->table;
    info->mark = 0;
    info->mask = 0xFFFFFFFF;
    info->priority = 0;
    
    struct rtattr *rta = (struct rtattr *)(frh + 1);
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*frh));
    
    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case FRA_FWMARK:
                info->mark = *(uint32_t *)RTA_DATA(rta);
                break;
            case FRA_FWMASK:
                info->mask = *(uint32_t *)RTA_DATA(rta);
                break;
            case FRA_PRIORITY:
                info->priority = *(uint32_t *)RTA_DATA(rta);
                break;
        }
    }
    
    pctx->count++;
    return 0;
}

int netlink_init(nl_callback_t callback, void *userdata) {
    g_sync_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (g_sync_fd < 0) {
        LOG_ERROR("Netlink: sync socket failed: %s", strerror(errno));
        return -1;
    }
    
    g_async_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (g_async_fd < 0) {
        LOG_ERROR("Netlink: async socket failed: %s", strerror(errno));
        close(g_sync_fd);
        g_sync_fd = -1;
        return -1;
    }
    
    int opt = 1;
    setsockopt(g_async_fd, SOL_NETLINK, NETLINK_EXT_ACK, &opt, sizeof(opt));
    
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | 
                     RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_LINK
    };
    
    if (bind(g_async_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Netlink: async bind failed: %s", strerror(errno));
        close(g_sync_fd);
        close(g_async_fd);
        g_sync_fd = -1;
        g_async_fd = -1;
        return -1;
    }
    
    g_callback = callback;
    g_userdata = userdata;
    g_seq = time(NULL);
    
    LOG_INFO("Netlink initialized (sync_fd=%d, async_fd=%d)", g_sync_fd, g_async_fd);
    return 0;
}

void netlink_cleanup(void) {
    if (g_sync_fd >= 0) close(g_sync_fd);
    if (g_async_fd >= 0) close(g_async_fd);
    g_sync_fd = -1;
    g_async_fd = -1;
    g_callback = NULL;
    LOG_INFO("Netlink cleaned up");
}

int netlink_get_fd(void) {
    return g_async_fd;
}

void netlink_handle_event(int fd, void *data) {
    (void)data;
    
    char buf[NL_BUFFER_SIZE];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (len < 0) {
        if (errno == ENOBUFS) {
            LOG_WARN("Netlink: ENOBUFS, events may be lost");
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("Netlink: recvmsg failed: %s", strerror(errno));
        }
        return;
    }
    
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
        switch (nlh->nlmsg_type) {
            case RTM_NEWLINK: {
                struct ifinfomsg *ifi = NLMSG_DATA(nlh);
                struct rtattr *rta = IFLA_RTA(ifi);
                int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifi));
                char ifname[IFNAMSIZ] = {0};
                
                for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
                    if (rta->rta_type == IFLA_IFNAME) {
                        strncpy(ifname, RTA_DATA(rta), IFNAMSIZ - 1);
                        break;
                    }
                }
                
                if (ifname[0]) {
                    int is_up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
                    LOG_DEBUG("Netlink: %s is %s", ifname, is_up ? "UP" : "DOWN");
                    if (g_callback) {
                        g_callback(is_up ? NL_EVENT_LINK_UP : NL_EVENT_LINK_DOWN, ifname, g_userdata);
                        if (is_vpn_interface(ifname)) {
                            g_callback(is_up ? NL_EVENT_VPN_CONNECTED : NL_EVENT_VPN_DISCONNECTED, 
                                       ifname, g_userdata);
                        }
                    }
                }
                break;
            }
            case RTM_NEWADDR: {
                struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
                char ifname[IFNAMSIZ];
                if (if_indextoname(ifa->ifa_index, ifname) && g_callback) {
                    g_callback(NL_EVENT_ADDR_ADD, ifname, g_userdata);
                }
                break;
            }
            case RTM_DELADDR: {
                struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
                char ifname[IFNAMSIZ];
                if (if_indextoname(ifa->ifa_index, ifname) && g_callback) {
                    g_callback(NL_EVENT_ADDR_DEL, ifname, g_userdata);
                }
                break;
            }
            case RTM_NEWROUTE:
                if (g_callback) g_callback(NL_EVENT_ROUTE_ADD, NULL, g_userdata);
                break;
            case RTM_DELROUTE:
                if (g_callback) g_callback(NL_EVENT_ROUTE_DEL, NULL, g_userdata);
                break;
        }
    }
}

int netlink_get_iface_stats(const char *iface, uint64_t *rx_bytes, uint64_t *tx_bytes) {
    struct nl_link_info links[64];
    struct nl_parse_ctx ctx = {
        .data.links = links,
        .max_count = 64,
        .count = 0,
        .error = 0
    };
    
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {0};
    
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST;
    req.nlh.nlmsg_seq = ++g_seq;
    req.ifi.ifi_index = if_nametoindex(iface);
    
    if (req.ifi.ifi_index == 0) return -1;
    
    pthread_mutex_lock(&g_mutex);
    
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }
    
    netlink_recv_multi(g_sync_fd, g_seq, parse_link, &ctx);
    pthread_mutex_unlock(&g_mutex);
    
    if (ctx.count > 0) {
        *rx_bytes = links[0].rx_bytes;
        *tx_bytes = links[0].tx_bytes;
        return 0;
    }
    
    return -1;
}

int netlink_get_active_vpn(char *iface, size_t size) {
    struct nl_link_info links[64];
    struct nl_parse_ctx ctx = {
        .data.links = links,
        .max_count = 64,
        .count = 0,
        .error = 0
    };
    
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {0};
    
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = ++g_seq;
    req.ifi.ifi_family = AF_PACKET;
    
    pthread_mutex_lock(&g_mutex);
    
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }
    
    netlink_recv_multi(g_sync_fd, g_seq, parse_link, &ctx);
    pthread_mutex_unlock(&g_mutex);
    
    for (int i = 0; i < ctx.count; i++) {
        if ((links[i].flags & IFF_UP) && is_vpn_interface(links[i].name)) {
            strncpy(iface, links[i].name, size - 1);
            iface[size - 1] = '\0';
            return 0;
        }
    }
    
    return -1;
}

int netlink_get_ipv4_snapshot(char *output, size_t size) {
    struct nl_addr_info addrs[128];
    struct nl_parse_ctx ctx = {
        .data.addrs = addrs,
        .max_count = 128,
        .count = 0,
        .error = 0
    };
    
    struct {
        struct nlmsghdr nlh;
        struct ifaddrmsg ifa;
    } req = {0};
    
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifa));
    req.nlh.nlmsg_type = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = ++g_seq;
    req.ifa.ifa_family = AF_INET;
    
    pthread_mutex_lock(&g_mutex);
    
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }
    
    netlink_recv_multi(g_sync_fd, g_seq, parse_addr, &ctx);
    pthread_mutex_unlock(&g_mutex);
    
    output[0] = '\0';
    for (int i = 0; i < ctx.count; i++) {
        if (addrs[i].family == AF_INET) {
            char ifname[IFNAMSIZ];
            if (if_indextoname(addrs[i].ifindex, ifname)) {
                char addr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addrs[i].addr.v4, addr_str, sizeof(addr_str));
                char buf[128];
                snprintf(buf, sizeof(buf), "%s:%s ", ifname, addr_str);
                strncat(output, buf, size - strlen(output) - 1);
            }
        }
    }
    
    return 0;
}

int netlink_check_rule_exists(int table_id, int mark, const char *iface) {
    (void)iface;
    
    struct nl_rule_info rules[64];
    struct nl_parse_ctx ctx = {
        .data.rules = rules,
        .max_count = 64,
        .count = 0,
        .error = 0
    };
    
    struct {
        struct nlmsghdr nlh;
        struct fib_rule_hdr frh;
    } req = {0};
    
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.frh));
    req.nlh.nlmsg_type = RTM_GETRULE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = ++g_seq;
    req.frh.family = AF_INET;
    
    pthread_mutex_lock(&g_mutex);
    
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }
    
    netlink_recv_multi(g_sync_fd, g_seq, parse_rule, &ctx);
    pthread_mutex_unlock(&g_mutex);
    
    for (int i = 0; i < ctx.count; i++) {
        if (rules[i].table == (uint32_t)table_id && 
            (rules[i].mark & rules[i].mask) == ((uint32_t)mark & rules[i].mask)) {
            return 1;
        }
    }
    
    return 0;
}

void netlink_monitor_handle(void) {
    netlink_handle_event(g_monitor_sock, NULL);
}
