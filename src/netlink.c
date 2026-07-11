#include "atpd_global.h"
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink Implementation - XFRM-aware VPN detection
 * Uses NETLINK_XFRM + getifaddrs() fallback for Google VPN (ipsec) detection
 */

#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include "atpd_context.h"

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
#include <linux/xfrm.h>
#include <pthread.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <sys/select.h>

#define NL_BUF_SIZE 16384
#define NL_DUMP_SIZE 65536
#define NL_RECV_TIMEOUT_MS 3000

/* ========== Firewall Self-Healing ========== */

#include "tproxy.h"

static int g_tproxy_initialized = 0;

void netlink_set_tproxy_ready(void) {
    g_tproxy_initialized = 1;
}

/* ========== Network Refresh Debounce ========== */

static reactor_timer_t *g_debounce_timer = NULL;
static reactor_t *g_debounce_reactor = NULL;

#define NETLINK_DEBOUNCE_MS 500

static pthread_mutex_t g_nl_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef IPTABLES_CMD
#define IPTABLES_CMD "/system/bin/iptables"
#endif
#ifndef IP6TABLES_CMD
#define IP6TABLES_CMD "/system/bin/ip6tables"
#endif

static int ip_rule_audit(atp_config_t *cfg);
static int tproxy_refresh_rules(atp_config_t *cfg);

static void debounce_flush_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;

    pthread_mutex_lock(&g_nl_mutex);

    ip_rule_audit(&g_config);
    tproxy_refresh_rules(&g_config);

    pthread_mutex_unlock(&g_nl_mutex);
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

/* ========== XFRM Interface Detection ========== */

#ifndef IFLA_XFRM_IF_ID
#define IFLA_XFRM_IF_ID    41
#endif

#ifndef XFRMA_RTA
#define XFRMA_RTA(x)  ((struct rtattr *)(((char *)(x)) + NLMSG_ALIGN(sizeof(struct xfrm_usersa_info))))
#endif
#ifndef XFRM_PAYLOAD
#define XFRM_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct xfrm_usersa_info))
#endif

static int g_xfrm_fd = -1;
static reactor_t *g_xfrm_reactor = NULL;
static int g_xfrm_registered = 0;

static struct rtattr* atpd_find_rta_nested(struct rtattr *rta, int len, unsigned short type) {
    while (RTA_OK(rta, len)) {
        if (rta->rta_type == type) return rta;
        rta = RTA_NEXT(rta, len);
    }
    return NULL;
}

static int detect_xfrm_interface(struct nlmsghdr *h) {
    struct ifinfomsg *ifi = NLMSG_DATA(h);
    struct rtattr *rta = IFLA_RTA(ifi);
    int len = IFLA_PAYLOAD(h);

    struct rtattr *li = atpd_find_rta_nested(rta, len, IFLA_LINKINFO);
    if (!li) return 0;

    struct rtattr *id = atpd_find_rta_nested(RTA_DATA(li), RTA_PAYLOAD(li), IFLA_INFO_DATA);
    if (!id) return 0;

    struct rtattr *xfrm_id = atpd_find_rta_nested(RTA_DATA(id), RTA_PAYLOAD(id), IFLA_XFRM_IF_ID);
    return (xfrm_id != NULL);
}

/* ========== getifaddrs Fallback for XFRM Tunnels ========== */

static int is_proxy_interface(const char *ifname);
static int getifaddrs_find_vpn(char *output, size_t size) {
    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) < 0) return -1;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (!is_proxy_interface(ifa->ifa_name)) continue;

        snprintf(output, size, "%s", ifa->ifa_name);
        freeifaddrs(ifaddr);
        return 0;
    }

    freeifaddrs(ifaddr);
    return -1;
}

/* ========== Netlink State ========== */

static int g_sync_fd = -1;
static int g_async_fd = -1;
static nl_callback_t g_callback = NULL;
static void *g_userdata = NULL;
static uint32_t g_seq = 1;

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

    if (strncmp(ifname, "ipsec", 5) == 0) {
        const char *p = ifname + 5;
        if (*p >= '0' && *p <= '9') return 1;
        return 0;
    }

    static const char *prefixes[] = {"tun", "wg", "vpn", "ppp"};
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

    if (detect_xfrm_interface(h)) {
        info->flags |= IFF_UP;
    }

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

static int netlink_recv_all_with_timeout(int fd, uint32_t seq,
                                         int (*parser)(struct nlmsghdr *, void *),
                                         void *ctx, int timeout_ms) {
    char *buf = malloc(NL_DUMP_SIZE);
    if (!buf) return -ENOMEM;

    struct sockaddr_nl nladdr;
    struct iovec iov = { buf, NL_DUMP_SIZE };
    struct msghdr msg = {
        .msg_name = &nladdr,
        .msg_namelen = sizeof(nladdr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    int status = 0;

    while (1) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            status = -errno;
            break;
        }
        if (ret == 0) {
            status = -ETIMEDOUT;
            LOG_WARN("netlink: recv timeout after %dms", timeout_ms);
            break;
        }

        ssize_t len = recvmsg(fd, &msg, 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            status = -errno;
            break;
        }

        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, (uint32_t)len); h = NLMSG_NEXT(h, len)) {
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

/* ========== Core Netlink API ========== */

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

    netlink_xfrm_init(NULL);

    LOG_INFO("Netlink: Engine started");
    return 0;
}

void netlink_handle_event(int fd, void *data) {
    (void)data;
    uint8_t buf[NL_BUF_SIZE];
    struct sockaddr_nl sa;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &sa,
        .msg_namelen = sizeof(sa),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

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

/* ========== XFRM Listener ========== */

int netlink_xfrm_init(reactor_t *r) {
    g_xfrm_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_XFRM);
    if (g_xfrm_fd < 0) {
        LOG_ERROR("XFRM: socket(NETLINK_XFRM) failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_groups = XFRMGRP_SA | XFRMGRP_POLICY
    };

    if (bind(g_xfrm_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_ERROR("XFRM: bind failed: %s", strerror(errno));
        close(g_xfrm_fd);
        g_xfrm_fd = -1;
        return -1;
    }

    if (r) {
        reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL);
        g_xfrm_reactor = r;
        g_xfrm_registered = 1;
    }

    LOG_INFO("XFRM: Listener started (fd=%d)", g_xfrm_fd);
    return 0;
}

void netlink_xfrm_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    (void)userdata;

    uint8_t buf[NL_BUF_SIZE];
    struct sockaddr_nl sa;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &sa,
        .msg_namelen = sizeof(sa),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

    ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == XFRM_MSG_NEWSA) {
            struct xfrm_usersa_info *sa = NLMSG_DATA(h);
            struct rtattr *rta = XFRMA_RTA(sa);
            int attr_len = XFRM_PAYLOAD(h);

            for (; RTA_OK(rta, attr_len); rta = RTA_NEXT(rta, attr_len)) {
                if (rta->rta_type == XFRMA_IF_ID) {
                    uint32_t if_id = *(uint32_t *)RTA_DATA(rta);
                    char ifname[IFNAMSIZ] = {0};
                    snprintf(ifname, sizeof(ifname), "ipsec%u", if_id - 1);

                    atpd_vpn_state_transition(VPN_STATE_PREDICTING, if_id, ifname);
                    trigger_network_refresh(g_debounce_reactor);
                    break;
                }
            }
        } else if (h->nlmsg_type == XFRM_MSG_DELSA) {
            atpd_vpn_state_transition(VPN_STATE_TEARDOWN, 0, NULL);
            trigger_network_refresh(g_debounce_reactor);
        }
    }
}


/* ========== VPN Detection ========== */

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
        netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                      parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
    }
    pthread_mutex_unlock(&g_nl_mutex);

    for (int i = 0; i < ctx.count; i++) {
        if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
            snprintf(output, size, "%s", links[i].name);
            return 0;
        }
    }

    return getifaddrs_find_vpn(output, size);
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
        netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                      parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
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
    if (g_debounce_timer && g_debounce_reactor) {
        reactor_cancel_timer(g_debounce_reactor, g_debounce_timer);
        g_debounce_timer = NULL;
    }
    g_debounce_reactor = NULL;
    g_xfrm_registered = 0;

    if (g_sync_fd >= 0) {
        close(g_sync_fd);
        g_sync_fd = -1;
    }
    if (g_async_fd >= 0) {
        close(g_async_fd);
        g_async_fd = -1;
    }
    if (g_xfrm_fd >= 0) {
        if (g_xfrm_reactor) {
            reactor_remove_fd(g_xfrm_reactor, g_xfrm_fd);
        }
        close(g_xfrm_fd);
        g_xfrm_fd = -1;
    }
    g_callback = NULL;
    g_xfrm_reactor = NULL;
}

int netlink_get_fd(void) {
    return g_async_fd;
}

void netlink_set_reactor(reactor_t *r) {
    g_debounce_reactor = r;

    if (g_xfrm_fd >= 0 && r && !g_xfrm_registered) {
        reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL);
        g_xfrm_reactor = r;
        g_xfrm_registered = 1;
    }
}

/* ========== VPN Detection Functions ========== */

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
    netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                  parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
    pthread_mutex_unlock(&g_nl_mutex);

    for (int i = 0; i < ctx.count; i++) {
        if (!(links[i].flags & IFF_UP)) continue;
        if (is_proxy_interface(links[i].name)) {
            LOG_DEBUG("VPN interface detected via netlink: %s", links[i].name);
            return 1;
        }
    }

    char ifname[IFNAMSIZ];
    if (getifaddrs_find_vpn(ifname, sizeof(ifname)) == 0) {
        LOG_DEBUG("VPN interface detected via getifaddrs: %s", ifname);
        return 1;
    }

    LOG_DEBUG("No VPN interface detected");
    return 0;
}

int nl_link_get_vpn_interface(char *iface, size_t size) {
    if (!iface || size == 0) return -1;

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
    netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                  parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
    pthread_mutex_unlock(&g_nl_mutex);

    for (int i = 0; i < ctx.count; i++) {
        if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
            strncpy(iface, links[i].name, size - 1);
            iface[size - 1] = '\0';
            LOG_DEBUG("Found VPN interface via netlink: %s", iface);
            return 0;
        }
    }

    return getifaddrs_find_vpn(iface, size);
}

/* ========== Firewall Self-Healing ========== */

static int ip_rule_audit(atp_config_t *cfg) {
    if (!g_tproxy_initialized) return 0;
    if (cfg->dry_run) return 0;

    char cmd[MAX_CMD_LEN];
    int needs_repair = 0;

    snprintf(cmd, sizeof(cmd),
             "ip rule show | grep -q 'fwmark %d lookup %d' 2>/dev/null",
             cfg->mark_value, cfg->table_id);

    if (exec_cmd_simple(cmd, 2) != 0) {
        LOG_WARN("IPv4 fwmark rule missing, repairing...");
        snprintf(cmd, sizeof(cmd),
                 "ip rule add fwmark %d table %d 2>/dev/null",
                 cfg->mark_value, cfg->table_id);
        exec_cmd_simple(cmd, 2);
        needs_repair = 1;
    }

    if (cfg->proxy_ipv6) {
        snprintf(cmd, sizeof(cmd),
                 "ip -6 rule show | grep -q 'fwmark %d lookup %d' 2>/dev/null",
                 cfg->mark_value6, cfg->table_id);

        if (exec_cmd_simple(cmd, 2) != 0) {
            LOG_WARN("IPv6 fwmark rule missing, repairing...");
            snprintf(cmd, sizeof(cmd),
                     "ip -6 rule add fwmark %d table %d 2>/dev/null",
                     cfg->mark_value6, cfg->table_id);
            exec_cmd_simple(cmd, 2);
            needs_repair = 1;
        }
    }

    if (needs_repair) {
        LOG_INFO("IP rule audit: repaired fwmark rules");
    }
    return 0;
}

static int tproxy_refresh_rules(atp_config_t *cfg) {
    if (!g_tproxy_initialized) return 0;
    if (cfg->dry_run) return 0;

    char cmd[MAX_CMD_LEN];
    int needs_repair_v4 = 0;
    int needs_repair_v6 = 0;

    snprintf(cmd, sizeof(cmd),
             "%s -t mangle -L PREROUTING 2>/dev/null | head -2 | grep -q ATP_PRE_0",
             IPTABLES_CMD);
    if (exec_cmd_simple(cmd, 2) != 0) {
        needs_repair_v4 = 1;
    }

    snprintf(cmd, sizeof(cmd),
             "%s -t mangle -L OUTPUT 2>/dev/null | head -2 | grep -q ATP_OUT_0",
             IPTABLES_CMD);
    if (exec_cmd_simple(cmd, 2) != 0) {
        needs_repair_v4 = 1;
    }

    if (needs_repair_v4) {
        LOG_INFO("TPROXY audit: repairing IPv4 hooks...");
        tproxy_hook_main_chains(cfg, 4, "");
    }

    if (cfg->proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "%s -t mangle -L PREROUTING 2>/dev/null | head -2 | grep -q ATP6_PRE_0",
                 IP6TABLES_CMD);
        if (exec_cmd_simple(cmd, 2) != 0) {
            needs_repair_v6 = 1;
        }

        snprintf(cmd, sizeof(cmd),
                 "%s -t mangle -L OUTPUT 2>/dev/null | head -2 | grep -q ATP6_OUT_0",
                 IP6TABLES_CMD);
        if (exec_cmd_simple(cmd, 2) != 0) {
            needs_repair_v6 = 1;
        }

        if (needs_repair_v6) {
            tproxy_hook_main_chains(cfg, 6, "6");
        }
    }

    return 0;
}
