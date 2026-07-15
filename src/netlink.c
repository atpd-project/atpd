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
#include <stdatomic.h>
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

static atomic_int g_tproxy_initialized = 0;

void netlink_set_tproxy_ready(void) {
    atomic_store(&g_tproxy_initialized, 1);
}

/* ========== Network Refresh Debounce ========== */

static reactor_timer_t *g_debounce_timer = NULL;
static reactor_t *g_debounce_reactor = NULL;
static pthread_mutex_t g_debounce_lock = PTHREAD_MUTEX_INITIALIZER;

#define NETLINK_DEBOUNCE_MS 500

static pthread_mutex_t g_nl_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_seq = 1;

#ifndef IPTABLES_CMD
#define IPTABLES_CMD "/system/bin/iptables"
#endif
#ifndef IP6TABLES_CMD
#define IP6TABLES_CMD "/system/bin/ip6tables"
#endif

static int ip_rule_audit(atp_config_t *cfg);
static int tproxy_refresh_rules(atp_config_t *cfg);

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void debounce_flush_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;

    ip_rule_audit(&g_config);
    tproxy_refresh_rules(&g_config);

    pthread_mutex_lock(&g_debounce_lock);
    g_debounce_timer = NULL;
    pthread_mutex_unlock(&g_debounce_lock);
}

static void trigger_network_refresh(reactor_t *r) {
    if (!r) return;

    pthread_mutex_lock(&g_debounce_lock);

    g_debounce_reactor = r;

    if (g_debounce_timer) {
        reactor_cancel_timer(r, g_debounce_timer);
        g_debounce_timer = NULL;
    }

    g_debounce_timer = reactor_add_timer(r, NETLINK_DEBOUNCE_MS, 0,
                                         debounce_flush_cb, NULL);
    LOG_DEBUG("[NET] Debounce timer (re)started: %dms", NETLINK_DEBOUNCE_MS);

    pthread_mutex_unlock(&g_debounce_lock);
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
static atomic_int g_xfrm_registered = 0;

static const struct rtattr* atpd_find_rta_nested(const struct rtattr *rta, int len, unsigned short type) {
    while (RTA_OK(rta, len)) {
        if (rta->rta_type == type) return rta;
        rta = RTA_NEXT(rta, len);
    }
    return NULL;
}

static const struct rtattr* atpd_find_attr(const struct rtattr *parent, unsigned short type) {
    if (!parent) return NULL;

    int len = RTA_PAYLOAD(parent);
    if (len < (int)sizeof(struct rtattr)) {
        return NULL;
    }

    const struct rtattr *rta = (const struct rtattr *)RTA_DATA(parent);
    return atpd_find_rta_nested(rta, len, type);
}

static const struct rtattr* atpd_find_nested_attr(const struct rtattr *parent, unsigned short type) {
    const struct rtattr *attr = atpd_find_attr(parent, type);
    if (!attr) return NULL;

    int len = RTA_PAYLOAD(attr);
    if (len < (int)sizeof(struct rtattr)) {
        return NULL;
    }

    const struct rtattr *nested = (const struct rtattr *)RTA_DATA(attr);
    if (!RTA_OK(nested, len)) {
        return NULL;
    }

    return attr;
}

static int detect_xfrm_interface(struct nlmsghdr *h) {
    if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) {
        return 0;
    }

    struct ifinfomsg *ifi = NLMSG_DATA(h);
    struct rtattr *rta = IFLA_RTA(ifi);
    int len = IFLA_PAYLOAD(h);

    const struct rtattr *linkinfo = atpd_find_rta_nested(rta, len, IFLA_LINKINFO);
    if (!linkinfo) return 0;

    const struct rtattr *info_data = atpd_find_nested_attr(linkinfo, IFLA_INFO_DATA);
    if (!info_data) return 0;

    const struct rtattr *xfrm_id = atpd_find_attr(info_data, IFLA_XFRM_IF_ID);
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

static void safe_copy_ifname(char *dest, const void *src, size_t src_len) {
    if (src_len == 0) {
        dest[0] = '\0';
        return;
    }

    size_t name_len = strnlen((const char *)src, src_len);
    size_t copy_len = (name_len < IFNAMSIZ - 1) ? name_len : IFNAMSIZ - 1;

    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static int parser_link_sync(struct nlmsghdr *h, void *ctx) {
    if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) {
        LOG_WARN("netlink: short RTM_GETLINK message");
        return -1;
    }

    struct nl_parse_ctx *pctx = (struct nl_parse_ctx *)ctx;
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(h);
    struct rtattr *rta = IFLA_RTA(ifi);
    int len = (int)IFLA_PAYLOAD(h);

    if (pctx->count >= pctx->max_count) return -1;
    struct nl_link_info *info = &pctx->links[pctx->count];
    info->index = ifi->ifi_index;
    info->flags = ifi->ifi_flags;
    info->rx_bytes = 0;
    info->tx_bytes = 0;

    if (detect_xfrm_interface(h)) {
        info->flags |= IFF_UP;
    }

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == IFLA_IFNAME) {
            safe_copy_ifname(info->name, RTA_DATA(rta), RTA_PAYLOAD(rta));
        } else if (rta->rta_type == IFLA_STATS64) {
            if (RTA_PAYLOAD(rta) >= sizeof(struct rtnl_link_stats64)) {
                struct rtnl_link_stats64 *s = (struct rtnl_link_stats64 *)RTA_DATA(rta);
                info->rx_bytes = s->rx_bytes;
                info->tx_bytes = s->tx_bytes;
            }
        }
    }
    if (info->name[0]) pctx->count++;
    return 0;
}

static int netlink_send_request(int fd, const void *buf, size_t len) {
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK
    };
    struct iovec iov = {
        .iov_base = (void *)buf,
        .iov_len = len
    };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

    ssize_t n = sendmsg(fd, &msg, 0);
    if (n != (ssize_t)len) {
        LOG_ERROR("netlink: short send (%zd/%zu)", n, len);
        return -1;
    }

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
    uint64_t start_ms = get_monotonic_ms();

    while (1) {
        uint64_t elapsed = get_monotonic_ms() - start_ms;
        if (elapsed >= (uint64_t)timeout_ms) {
            LOG_WARN("netlink: overall timeout after %dms", timeout_ms);
            status = -ETIMEDOUT;
            break;
        }

        uint64_t remain = (uint64_t)timeout_ms - elapsed;

        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;

        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            status = -errno;
            break;
        }
        if (ret == 0) {
            LOG_WARN("netlink: select timeout, remaining %llums", (unsigned long long)remain);
            status = -ETIMEDOUT;
            break;
        }

        ssize_t len = recvmsg(fd, &msg, 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            status = -errno;
            break;
        }

        if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
            LOG_ERROR("netlink: message truncated");
            status = -EOVERFLOW;
            break;
        }

        if (nladdr.nl_pid != 0) {
            LOG_WARN("netlink: unexpected sender pid=%u", nladdr.nl_pid);
            continue;
        }

        int multipart = 0;
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, (uint32_t)len); h = NLMSG_NEXT(h, len)) {
            multipart = (h->nlmsg_flags & NLM_F_MULTI);
            if (h->nlmsg_seq != seq) continue;

            if (h->nlmsg_type == NLMSG_DONE) {
                goto done;
            }

            if (h->nlmsg_type == NLMSG_ERROR) {
                if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                    LOG_ERROR("netlink: malformed NLMSG_ERROR");
                    status = -EINVAL;
                    goto done;
                }
                status = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
                goto done;
            }

            if (parser && parser(h, ctx) < 0) goto done;
        }

        if (!multipart) {
            break;
        }
    }
done:
    free(buf);
    return status;
}

/* ========== Core Netlink API ========== */

int netlink_init(nl_callback_t callback, void *userdata) {
    (void)callback;
    (void)userdata;
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

    atomic_store(&g_seq, (uint32_t)time(NULL));

    if (netlink_xfrm_init(NULL) != 0) {
        close(g_sync_fd);
        close(g_async_fd);
        g_sync_fd = -1;
        g_async_fd = -1;
        return -1;
    }

    LOG_INFO("Netlink: Engine started");
    return 0;
}

void netlink_handle_event(int fd, void *data) {
    (void)data;
    uint8_t buf[NL_BUF_SIZE];
    struct sockaddr_nl nladdr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &nladdr,
        .msg_namelen = sizeof(nladdr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

    ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;

    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        LOG_WARN("[NET] truncated netlink message");
        return;
    }

    if (nladdr.nl_pid != 0) {
        LOG_WARN("[NET] unexpected sender pid=%u", nladdr.nl_pid);
        return;
    }

    int should_refresh = 0;

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == NLMSG_DONE) break;
        if (h->nlmsg_type == NLMSG_ERROR) continue;

        if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_NEWROUTE) {
            char ifname[IFNAMSIZ] = {0};

            if (h->nlmsg_type == RTM_NEWADDR) {
                if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifaddrmsg))) {
                    LOG_WARN("[NET] RTM_NEWADDR message too short");
                    continue;
                }
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
        atomic_store(&g_xfrm_registered, 1);
    }

    LOG_INFO("XFRM: Listener started (fd=%d)", g_xfrm_fd);
    return 0;
}

void netlink_xfrm_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    (void)userdata;

    uint8_t buf[NL_BUF_SIZE];
    struct sockaddr_nl nladdr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &nladdr,
        .msg_namelen = sizeof(nladdr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

    ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;

    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        LOG_WARN("[XFRM] truncated netlink message");
        return;
    }

    if (nladdr.nl_pid != 0) {
        LOG_WARN("[XFRM] unexpected sender pid=%u", nladdr.nl_pid);
        return;
    }

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == XFRM_MSG_NEWSA) {
            if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct xfrm_usersa_info))) {
                LOG_WARN("[XFRM] XFRM_MSG_NEWSA message too short");
                continue;
            }

            struct xfrm_usersa_info *sa_info = NLMSG_DATA(h);
            struct rtattr *rta = XFRMA_RTA(sa_info);
            int attr_len = XFRM_PAYLOAD(h);

            for (; RTA_OK(rta, attr_len); rta = RTA_NEXT(rta, attr_len)) {
                if (rta->rta_type == XFRMA_IF_ID) {
                    if (RTA_PAYLOAD(rta) < sizeof(uint32_t)) {
                        LOG_WARN("[XFRM] IF_ID attribute too short");
                        continue;
                    }
                    uint32_t if_id;
                    memcpy(&if_id, RTA_DATA(rta), sizeof(if_id));

                    if (if_id == 0) {
                        LOG_DEBUG("[XFRM] if_id=0, skipping");
                        continue;
                    }

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
            .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    pthread_mutex_lock(&g_nl_mutex);

    if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        return -1;
    }

    netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                  parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
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
            .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
        },
        .ifi = { .ifi_index = if_nametoindex(iface) }
    };

    if (req.ifi.ifi_index == 0) return -1;

    pthread_mutex_lock(&g_nl_mutex);

    if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        return -1;
    }

    netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                  parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
    pthread_mutex_unlock(&g_nl_mutex);

    if (ctx.count > 0) {
        *rx = links[0].rx_bytes;
        *tx = links[0].tx_bytes;
        return 0;
    }
    return -1;
}

void netlink_cleanup(void) {
    pthread_mutex_lock(&g_debounce_lock);
    if (g_debounce_timer && g_debounce_reactor) {
        reactor_cancel_timer(g_debounce_reactor, g_debounce_timer);
        g_debounce_timer = NULL;
    }
    g_debounce_reactor = NULL;
    pthread_mutex_unlock(&g_debounce_lock);

    if (g_xfrm_fd >= 0) {
        if (g_xfrm_reactor) {
            reactor_remove_fd(g_xfrm_reactor, g_xfrm_fd);
        }
        close(g_xfrm_fd);
        g_xfrm_fd = -1;
    }

    atomic_store(&g_xfrm_registered, 0);
    g_xfrm_reactor = NULL;

    if (g_sync_fd >= 0) {
        close(g_sync_fd);
        g_sync_fd = -1;
    }
    if (g_async_fd >= 0) {
        close(g_async_fd);
        g_async_fd = -1;
    }
}

int netlink_get_fd(void) {
    return g_async_fd;
}

void netlink_set_reactor(reactor_t *r) {
    g_debounce_reactor = r;

    if (g_xfrm_fd >= 0 && r && !atomic_load(&g_xfrm_registered)) {
        reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL);
        g_xfrm_reactor = r;
        atomic_store(&g_xfrm_registered, 1);
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
            .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    pthread_mutex_lock(&g_nl_mutex);

    if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
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
            .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    pthread_mutex_lock(&g_nl_mutex);

    if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        iface[0] = '\0';
        return -1;
    }

    netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                  parser_link_sync, &ctx, NL_RECV_TIMEOUT_MS);
    pthread_mutex_unlock(&g_nl_mutex);

    for (int i = 0; i < ctx.count; i++) {
        if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
            snprintf(iface, size, "%s", links[i].name);
            LOG_DEBUG("Found VPN interface via netlink: %s", iface);
            return 0;
        }
    }

    return getifaddrs_find_vpn(iface, size);
}

/* ========== Firewall Self-Healing ========== */

static int ip_rule_audit(atp_config_t *cfg) {
    if (!atomic_load(&g_tproxy_initialized)) return 0;
    if (cfg->core.dry_run) return 0;

    char cmd[MAX_CMD_LEN];
    int needs_repair = 0;

    snprintf(cmd, sizeof(cmd),
             "ip rule show | grep -q 'fwmark %d lookup %d' 2>/dev/null",
             cfg->network.mark_value, cfg->network.table_id);

    if (exec_cmd_simple(cmd, 2) != 0) {
        LOG_WARN("IPv4 fwmark rule missing, repairing...");
        snprintf(cmd, sizeof(cmd),
                 "ip rule add fwmark %d table %d 2>/dev/null",
                 cfg->network.mark_value, cfg->network.table_id);
        exec_cmd_simple(cmd, 2);
        needs_repair = 1;
    }

    if (cfg->network.proxy_ipv6) {
        snprintf(cmd, sizeof(cmd),
                 "ip -6 rule show | grep -q 'fwmark %d lookup %d' 2>/dev/null",
                 cfg->network.mark_value6, cfg->network.table_id);

        if (exec_cmd_simple(cmd, 2) != 0) {
            LOG_WARN("IPv6 fwmark rule missing, repairing...");
            snprintf(cmd, sizeof(cmd),
                     "ip -6 rule add fwmark %d table %d 2>/dev/null",
                     cfg->network.mark_value6, cfg->network.table_id);
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
    if (!atomic_load(&g_tproxy_initialized)) return 0;
    if (cfg->core.dry_run) return 0;

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
        tproxy_hook_main_chains(cfg, 4);
    }

    if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
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
            tproxy_hook_main_chains(cfg, 6);
        }
    }

    return 0;
}
