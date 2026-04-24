/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink Implementation - Strictly optimized for musl-libc static linkage.
 * Zero-warning version for industrial deployment.
 */

#include "netlink.h"
#include "logger.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <pthread.h>

#define NL_BUF_SIZE 16384
#define NL_DUMP_SIZE 65536

/* ========== Network Refresh Debounce ========== */

static reactor_timer_t *g_debounce_timer = NULL;
static reactor_t *g_debounce_reactor = NULL;

#define NETLINK_DEBOUNCE_MS 500

static void debounce_flush_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    
    LOG_INFO("[NET] Debounce timer expired, executing network refresh");
    
    tproxy_refresh_rules();
    ip_rule_audit();
    
    g_debounce_timer = NULL;
}

static void trigger_network_refresh(reactor_t *r) {
    if (!r) return;
    g_debounce_reactor = r;
    
    if (g_debounce_timer) {
        reactor_cancel_timer(r, g_debounce_timer);
        g_debounce_timer = NULL;
    }
    
    g_debounce_timer = reactor_add_timer(r, NETLINK_DEBOUNCE_MS, 0, 
                                         debounce_flush_cb, NULL);
    LOG_DEBUG("[NET] Debounce timer (re)started: %dms", NETLINK_DEBOUNCE_MS);
}

static int g_sync_fd = -1;
static int g_async_fd = -1;
static nl_callback_t g_callback = NULL;
static void *g_userdata = NULL;
static uint32_t g_seq = 1;
static pthread_mutex_t g_nl_mutex = PTHREAD_MUTEX_INITIALIZER;

struct nl_link_info {
    int index;
    char name[IFNAMSIZ];
    unsigned int flags;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
};

struct nl_parse_ctx {
    struct nl_link_info *links;
    int max_count;
    int count;
};

static int is_proxy_interface(const char *ifname) {
    if (!ifname) return 0;
    static const char *prefixes[] = {"tun", "wg", "ipsec", "vpn", "ppp", "dummy"};
    for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); ++i) {
        if (strncmp(ifname, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    }
    return 0;
}

static int parser_link_sync(struct nlmsghdr *h, void *ctx) {
    struct nl_parse_ctx *pctx = (struct nl_parse_ctx *)ctx;
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(h);
    struct rtattr *rta = IFLA_RTA(ifi);
    int len = (int)IFLA_PAYLOAD(h);

    if (pctx->count >= pctx->max_count) return -1;
    struct nl_link_info *info = &pctx->links[pctx->count];
    info->index = ifi->ifi_index;
    info->flags = ifi->ifi_flags;

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == IFLA_IFNAME) {
            snprintf(info->name, IFNAMSIZ, "%s", (char *)RTA_DATA(rta));
        } else if (rta->rta_type == IFLA_STATS64) {
            struct rtnl_link_stats64 *s = (struct rtnl_link_stats64 *)RTA_DATA(rta);
            info->rx_bytes = s->rx_bytes;
            info->tx_bytes = s->tx_bytes;
        }
    }
    if (info->name[0]) pctx->count++;
    return 0;
}

static int netlink_recv_all(int fd, uint32_t seq, int (*parser)(struct nlmsghdr *, void *), void *ctx) {
    char *buf = malloc(NL_DUMP_SIZE);
    if (!buf) return -ENOMEM;
    struct sockaddr_nl nladdr;
    struct iovec iov = { buf, NL_DUMP_SIZE };
    struct msghdr msg = { .msg_name = &nladdr, .msg_namelen = sizeof(nladdr), .msg_iov = &iov, .msg_iovlen = 1 };
    int status = 0;

    while (1) {
        ssize_t ret = recvmsg(fd, &msg, 0);
        if (ret < 0) {
            if (errno == EINTR) continue;
            status = -errno;
            break;
        }
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, (uint32_t)ret); h = NLMSG_NEXT(h, ret)) {
            if (h->nlmsg_seq != seq) continue;
            if (h->nlmsg_type == NLMSG_DONE) goto done;
            if (h->nlmsg_type == NLMSG_ERROR) {
                status = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
                goto done;
            }
            if (parser && parser(h, ctx) < 0) goto done;
        }
        if (!(h->nlmsg_flags & NLM_F_MULTI)) break;
    }
done:
    free(buf);
    return status;
}

int netlink_init(nl_callback_t callback, void *userdata) {
    g_sync_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    g_async_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (g_sync_fd < 0 || g_async_fd < 0) {
        if (g_sync_fd >= 0) close(g_sync_fd);
        if (g_async_fd >= 0) close(g_async_fd);
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | 
                     RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE
    };

    if (bind(g_async_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(g_sync_fd);
        close(g_async_fd);
        return -1;
    }

    g_callback = callback;
    g_userdata = userdata;
    g_seq = (uint32_t)time(NULL);

    LOG_INFO("Netlink: Engine started");
    return 0;
}

void netlink_handle_event(int fd, void *data) {
    (void)data;
    uint8_t buf[NL_BUF_SIZE];
    struct sockaddr_nl sa;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = { .msg_name = &sa, .msg_namelen = sizeof(sa), .msg_iov = &iov, .msg_iovlen = 1 };

    ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;

    int should_refresh = 0;

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf; 
         NLMSG_OK(h, (uint32_t)len); 
         h = NLMSG_NEXT(h, len)) {
        
        if (h->nlmsg_type == NLMSG_DONE) break;
        if (h->nlmsg_type == NLMSG_ERROR) continue;

        if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_NEWROUTE) {
            char ifname[IFNAMSIZ] = {0};
            
            if (h->nlmsg_type == RTM_NEWADDR) {
                struct ifaddrmsg *ifa = NLMSG_DATA(h);
                if_indextoname(ifa->ifa_index, ifname);
                LOG_DEBUG("[NET] Address change on %s", ifname[0] ? ifname : "unknown");
            } else {
                LOG_DEBUG("[NET] Route table change detected");
                ifname[0] = '\0';
            }
            
            should_refresh = 1;
        }
    }

    if (should_refresh) {
        trigger_network_refresh(g_debounce_reactor);
    }
}

int netlink_get_active_vpn(char *output, size_t size) {
    struct nl_link_info links[32] = {{0}};
    struct nl_parse_ctx ctx = { .links = links, .max_count = 32, .count = 0 };
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = ++g_seq
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    pthread_mutex_lock(&g_nl_mutex);
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) >= 0) {
        netlink_recv_all(g_sync_fd, req.nlh.nlmsg_seq, parser_link_sync, &ctx);
    }
    pthread_mutex_unlock(&g_nl_mutex);

    for (int i = 0; i < ctx.count; i++) {
        if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
            snprintf(output, size, "%s", links[i].name);
            return 0;
        }
    }
    return -1;
}

int netlink_get_iface_stats(const char *iface, uint64_t *rx, uint64_t *tx) {
    struct nl_link_info links[1] = {{0}};
    struct nl_parse_ctx ctx = { .links = links, .max_count = 1, .count = 0 };
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST,
            .nlmsg_seq = ++g_seq
        },
        .ifi = { .ifi_index = if_nametoindex(iface) }
    };

    if (req.ifi.ifi_index == 0) return -1;
    pthread_mutex_lock(&g_nl_mutex);
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) >= 0) {
        netlink_recv_all(g_sync_fd, req.nlh.nlmsg_seq, parser_link_sync, &ctx);
    }
    pthread_mutex_unlock(&g_nl_mutex);

    if (ctx.count > 0) {
        *rx = links[0].rx_bytes;
        *tx = links[0].tx_bytes;
        return 0;
    }
    return -1;
}

void netlink_cleanup(void) {
    if (g_sync_fd >= 0) {
        close(g_sync_fd);
        g_sync_fd = -1;
    }
    if (g_async_fd >= 0) {
        close(g_async_fd);
        g_async_fd = -1;
    }
    g_callback = NULL;
}

int netlink_get_fd(void) {
    return g_async_fd;
}

/* ========== VPN Detection Functions ========== */

/**
 * Detect if any VPN interface exists via netlink
 * This leverages the existing netlink infrastructure for consistency.
 * @return 1 if VPN detected, 0 if not, -1 on error
 */
int nl_vpn_detect(void) {
    struct nl_link_info links[32] = {{0}};
    struct nl_parse_ctx ctx = { .links = links, .max_count = 32, .count = 0 };
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = ++g_seq
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    pthread_mutex_lock(&g_nl_mutex);
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        LOG_ERROR("nl_vpn_detect: netlink send failed: %s", strerror(errno));
        return -1;
    }
    netlink_recv_all(g_sync_fd, req.nlh.nlmsg_seq, parser_link_sync, &ctx);
    pthread_mutex_unlock(&g_nl_mutex);

    // Iterate through all interfaces and check for VPN characteristics
    for (int i = 0; i < ctx.count; i++) {
        // Check if interface is UP
        if (!(links[i].flags & IFF_UP)) {
            continue;
        }

        // Use the existing is_proxy_interface() which already handles ipsec0, ipsec1, etc.
        if (is_proxy_interface(links[i].name)) {
            LOG_DEBUG("VPN interface detected via netlink: %s", links[i].name);
            return 1;
        }
    }

    LOG_DEBUG("No VPN interface detected via netlink");
    return 0;
}

/**
 * Get the VPN interface name via netlink
 * @param iface Buffer to store interface name
 * @param size  Size of buffer
 * @return 0 on success, -1 if no VPN interface
 */
int nl_link_get_vpn_interface(char *iface, size_t size) {
    if (!iface || size == 0) {
        return -1;
    }

    struct nl_link_info links[32] = {{0}};
    struct nl_parse_ctx ctx = { .links = links, .max_count = 32, .count = 0 };
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = ++g_seq
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    pthread_mutex_lock(&g_nl_mutex);
    if (send(g_sync_fd, &req, req.nlh.nlmsg_len, 0) < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        LOG_ERROR("nl_link_get_vpn_interface: netlink send failed: %s", strerror(errno));
        iface[0] = '\0';
        return -1;
    }
    netlink_recv_all(g_sync_fd, req.nlh.nlmsg_seq, parser_link_sync, &ctx);
    pthread_mutex_unlock(&g_nl_mutex);

    // Find first active VPN interface
    for (int i = 0; i < ctx.count; i++) {
        if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
            strncpy(iface, links[i].name, size - 1);
            iface[size - 1] = '\0';
            LOG_DEBUG("Found VPN interface via netlink: %s", iface);
            return 0;
        }
    }

    // No active VPN found
    iface[0] = '\0';
    LOG_DEBUG("No active VPN interface found via netlink");
    return -1;
}
