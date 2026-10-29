/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink Implementation - XFRM & Multi-VPN tunnel sensing (WARP, WireGuard, Tailscale, IPSec)
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
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/fib_rules.h>
#include <linux/xfrm.h>
#include <pthread.h>

#define NL_BUF_SIZE 8192
#define NL_DUMP_SIZE 32768
#define NETLINK_RECV_TIMEOUT_MS 3000
#define NETLINK_DEBOUNCE_MS 500
#define NETLINK_VPN_SETTLE_MS 1000

#ifndef XFRMA_RTA
#define XFRMA_RTA(r) ((struct rtattr*)((char*)(r) + NLMSG_ALIGN(sizeof(struct xfrm_usersa_info))))
#endif
#ifndef XFRM_PAYLOAD
#define XFRM_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct xfrm_usersa_info))
#endif
#ifndef XFRMA_IF_ID
#define XFRMA_IF_ID 28
#endif

static int g_sync_fd = -1;
static int g_async_fd = -1;
static int g_xfrm_fd = -1;
static atomic_int g_xfrm_registered = 0;
static reactor_t *g_xfrm_reactor = NULL;

static vpn_bypass_route_t g_bypass_routes[ATP_MAX_VPN_BYPASS_ROUTES];
static int g_bypass_route_count = 0;
static int g_bypass_pref = 12400;

static nl_callback_t g_callback = NULL;
static void *g_userdata = NULL;
static atomic_uint g_seq = 0;
static pthread_mutex_t g_nl_mutex = PTHREAD_MUTEX_INITIALIZER;
static void trigger_network_refresh_delay(reactor_t *r, int delay_ms);

static reactor_t *g_debounce_reactor = NULL;
static reactor_timer_t *g_debounce_timer = NULL;
static pthread_mutex_t g_debounce_lock = PTHREAD_MUTEX_INITIALIZER;

static void trigger_network_refresh(reactor_t *r);
static int is_proxy_interface(const char *ifname);
static void vpn_bypass_sync(int ready);

struct nl_link_info {
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

    if (strncmp(ifname, "ipsec", 5) == 0 || strncmp(ifname, "xfrm", 4) == 0) {
        return 1;
    }

    static const char *prefixes[] = {
        "tun", "warp", "wg", "tailscale", "zt", "zerotier", "utun", "vpn", "ppp"
    };
    for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); ++i) {
        if (strncmp(ifname, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    }
    return 0;
}

const char* netlink_get_vpn_type_label(const char *iface) {
    if (!iface || !iface[0]) return "None";
    if (strncmp(iface, "warp", 4) == 0) return "Cloudflare WARP";
    if (strncmp(iface, "tun", 3) == 0) return "Cloudflare WARP / TUN";
    if (strncmp(iface, "wg", 2) == 0) return "WireGuard";
    if (strncmp(iface, "tailscale", 9) == 0) return "Tailscale";
    if (strncmp(iface, "zt", 2) == 0 || strncmp(iface, "zerotier", 8) == 0) return "ZeroTier";
    if (strncmp(iface, "ipsec", 5) == 0) return "Google VPN / IPsec";
    if (strncmp(iface, "xfrm", 4) == 0) return "IPsec XFRM";
    if (strncmp(iface, "utun", 4) == 0) return "User TUN";
    if (strncmp(iface, "ppp", 3) == 0) return "PPP Tunnel";
    return "VPN Tunnel";
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

static int open_netlink_socket(int groups) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        if (groups != 0) {
            LOG_ERROR("Netlink: socket() failed: %s", strerror(errno));
        }
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_groups = (uint32_t)groups
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (groups != 0) {
            LOG_ERROR("Netlink: bind() failed: %s", strerror(errno));
        }
        close(fd);
        return -1;
    }

    int buf_size = NL_DUMP_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    return fd;
}

static void netlink_drain_socket(int fd) {
    if (fd < 0) return;
    char buf[1024];
    while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}

static int netlink_send_request(int fd, const void *req, size_t len) {
    if (fd < 0) return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    ssize_t sent = sendto(fd, req, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    if (sent < 0 || (size_t)sent != len) {
        LOG_ERROR("Netlink: sendto() failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

typedef int (*nl_msg_parser_t)(struct nlmsghdr *h, void *userdata);

static int netlink_recv_all_with_timeout(int fd, uint32_t expected_seq,
                                          nl_msg_parser_t parser, void *userdata,
                                          int timeout_ms) {
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char *buf = malloc(NL_DUMP_SIZE);
    if (!buf) return -1;

    int done = 0;
    while (!done) {
        ssize_t len = recv(fd, buf, NL_DUMP_SIZE, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN("Netlink: recv timeout");
            } else {
                LOG_ERROR("Netlink: recv failed: %s", strerror(errno));
            }
            free(buf);
            return -1;
        }

        for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
             NLMSG_OK(h, (uint32_t)len);
             h = NLMSG_NEXT(h, len)) {

            if (h->nlmsg_seq != expected_seq) continue;

            if (h->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }

            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);
                if (err->error != 0) {
                    if (err->error == -EEXIST) {
                        done = 1;
                        break;
                    }
                    LOG_ERROR("Netlink: error response %d: %s", err->error, strerror(-err->error));
                    free(buf);
                    return -1;
                }
                done = 1;
                break;
            }

            if (parser && parser(h, userdata) < 0) {
                free(buf);
                return -1;
            }
        }
    }

    free(buf);
    return 0;
}

static int parser_link_sync(struct nlmsghdr *h, void *userdata) {
    struct nl_parse_ctx *ctx = (struct nl_parse_ctx *)userdata;
    if (h->nlmsg_type != RTM_NEWLINK) return 0;
    if (ctx->count >= ctx->max_count) return 0;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(h);
    struct rtattr *rta = IFLA_RTA(ifi);
    int len = IFLA_PAYLOAD(h);

    struct nl_link_info *info = &ctx->links[ctx->count];
    memset(info, 0, sizeof(*info));
    info->flags = ifi->ifi_flags;

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == IFLA_IFNAME) {
            safe_copy_ifname(info->name, RTA_DATA(rta), RTA_PAYLOAD(rta));
        } else if (rta->rta_type == IFLA_STATS64) {
            if (RTA_PAYLOAD(rta) >= sizeof(struct rtnl_link_stats64)) {
                struct rtnl_link_stats64 stats;
                memcpy(&stats, RTA_DATA(rta), sizeof(stats));
                info->rx_bytes = stats.rx_bytes;
                info->tx_bytes = stats.tx_bytes;
            }
        }
    }

    if (info->name[0] != '\0') {
        ctx->count++;
    }
    return 0;
}

static int getifaddrs_find_vpn(char *iface, size_t size) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;

    const char *fallback = NULL;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !(ifa->ifa_flags & IFF_UP)) continue;
        if (!is_proxy_interface(ifa->ifa_name)) continue;
        if (strncmp(ifa->ifa_name, "ipsec", 5) == 0) {
            snprintf(iface, size, "%s", ifa->ifa_name);
            freeifaddrs(ifaddr);
            return 0;
        }
        if (!fallback) fallback = ifa->ifa_name;
    }

    if (fallback) snprintf(iface, size, "%s", fallback);
    freeifaddrs(ifaddr);
    return fallback ? 0 : -1;
}

int netlink_init(nl_callback_t callback, void *userdata) {
    pthread_mutex_lock(&g_nl_mutex);

    g_callback = callback;
    g_userdata = userdata;

    if (userdata) {
        const atp_config_t *cfg = (const atp_config_t *)userdata;
        g_bypass_route_count = cfg->interface.vpn_bypass_route_count;
        if (g_bypass_route_count > ATP_MAX_VPN_BYPASS_ROUTES) {
            g_bypass_route_count = ATP_MAX_VPN_BYPASS_ROUTES;
        }
        if (g_bypass_route_count > 0) {
            memcpy(g_bypass_routes, cfg->interface.vpn_bypass_routes,
                   sizeof(vpn_bypass_route_t) * g_bypass_route_count);
        } else {
            memset(g_bypass_routes, 0, sizeof(g_bypass_routes));
        }
        g_bypass_pref = cfg->interface.vpn_bypass_pref;
    } else {
        g_bypass_route_count = 0;
        memset(g_bypass_routes, 0, sizeof(g_bypass_routes));
        g_bypass_pref = 12400;
    }

    g_sync_fd = open_netlink_socket(0);
    if (g_sync_fd < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        return -1;
    }

    g_async_fd = open_netlink_socket(
        RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
        RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE |
        RTMGRP_IPV4_RULE | (1u << (RTNLGRP_IPV6_RULE - 1))
    );
    if (g_async_fd < 0) {
        close(g_sync_fd);
        g_sync_fd = -1;
        g_callback = NULL;
        g_userdata = NULL;
        pthread_mutex_unlock(&g_nl_mutex);
        return -1;
    }

    pthread_mutex_unlock(&g_nl_mutex);
    LOG_INFO("Netlink initialized (sync_fd=%d, async_fd=%d)", g_sync_fd, g_async_fd);
    return 0;
}

int netlink_xfrm_init(reactor_t *r) {
    if (g_xfrm_fd >= 0) {
        if (r && !atomic_load(&g_xfrm_registered)) {
            if (reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL) != 0) {
                LOG_ERROR("[XFRM] failed to register existing listener with reactor");
                return -1;
            }
            g_xfrm_reactor = r;
            atomic_store(&g_xfrm_registered, 1);
        }
        return g_xfrm_fd;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_XFRM);
    if (fd < 0) {
        LOG_WARN("[XFRM] socket(NETLINK_XFRM) failed: %s (Google VPN auto-detection disabled)", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_groups = XFRMNLGRP_SA
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_WARN("[XFRM] bind() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    int buf_size = NL_DUMP_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    g_xfrm_fd = fd;

    if (r) {
        if (reactor_add_fd(r, fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL) != 0) {
            LOG_ERROR("[XFRM] failed to register listener with reactor");
            close(fd);
            g_xfrm_fd = -1;
            return -1;
        }
        g_xfrm_reactor = r;
        atomic_store(&g_xfrm_registered, 1);
    }

    LOG_INFO("[XFRM] XFRM listener initialized on fd=%d", fd);
    return fd;
}

void netlink_xfrm_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;
    if (!(events & REACTOR_EVENT_READ)) return;

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

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == XFRM_MSG_NEWSA) {
            struct xfrm_usersa_info *sa_info = NLMSG_DATA(h);
            struct rtattr *rta = XFRMA_RTA(sa_info);
            int attr_len = XFRM_PAYLOAD(h);

            for (; RTA_OK(rta, attr_len); rta = RTA_NEXT(rta, attr_len)) {
                if (rta->rta_type == XFRMA_IF_ID) {
                    uint32_t if_id;
                    memcpy(&if_id, RTA_DATA(rta), sizeof(if_id));
                    if (if_id == 0) continue;

                    char ifname[IFNAMSIZ] = {0};
                    snprintf(ifname, sizeof(ifname), "ipsec%u", if_id - 1);

                    atpd_vpn_state_transition(VPN_STATE_PREDICTING, if_id, ifname);
                    trigger_network_refresh_delay(g_debounce_reactor, NETLINK_VPN_SETTLE_MS);
                    break;
                }
            }
        } else if (h->nlmsg_type == XFRM_MSG_DELSA) {
            atpd_vpn_state_transition(VPN_STATE_TEARDOWN, 0, NULL);
            trigger_network_refresh_delay(g_debounce_reactor, NETLINK_VPN_SETTLE_MS);
        }
    }
}

int netlink_get_active_vpn(char *output, size_t size) {
    if (!output || size == 0) return -1;
    output[0] = '\0';

    /* 1. Fast path: getifaddrs */
    if (getifaddrs_find_vpn(output, size) == 0 && output[0] != '\0') {
        return 0;
    }

    /* 2. Fast path: Netlink dump with auto-opened socket */
    int temp_fd = -1;
    int sync_fd = g_sync_fd;
    if (sync_fd < 0) {
        temp_fd = open_netlink_socket(0);
        sync_fd = temp_fd;
    }
    if (sync_fd < 0) {
        return -1;
    }

    struct nl_link_info links[32] = {0};
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

    int res = -1;
    pthread_mutex_lock(&g_nl_mutex);
    netlink_drain_socket(sync_fd);
    if (netlink_send_request(sync_fd, &req, req.nlh.nlmsg_len) == 0) {
        if (netlink_recv_all_with_timeout(sync_fd, req.nlh.nlmsg_seq,
                                          parser_link_sync, &ctx, 500) == 0) {
            int fallback = -1;
            for (int i = 0; i < ctx.count; i++) {
                if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
                    if (strncmp(links[i].name, "ipsec", 5) == 0) {
                        snprintf(output, size, "%s", links[i].name);
                        res = 0;
                        break;
                    }
                    if (fallback < 0) fallback = i;
                }
            }
            if (res != 0 && fallback >= 0) {
                snprintf(output, size, "%s", links[fallback].name);
                res = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_nl_mutex);

    if (temp_fd >= 0) {
        close(temp_fd);
    }

    return res;
}

int netlink_get_iface_stats(const char *iface, uint64_t *rx, uint64_t *tx) {
    if (!iface || !iface[0] || !rx || !tx) return -1;

    /* 1. Fast direct read from /proc/net/dev */
    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp) {
        char line[512];
        if (fgets(line, sizeof(line), fp) && fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                char name[64];
                unsigned long long rx_val = 0, tx_val = 0;
                if (sscanf(line, "%63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                           name, &rx_val, &tx_val) >= 3) {
                    char *p = name;
                    while (*p == ' ') p++;
                    if (strcmp(p, iface) == 0) {
                        *rx = (uint64_t)rx_val;
                        *tx = (uint64_t)tx_val;
                        fclose(fp);
                        return 0;
                    }
                }
            }
        }
        fclose(fp);
    }

    /* 2. Fallback to netlink */
    int temp_fd = -1;
    int sync_fd = g_sync_fd;
    if (sync_fd < 0) {
        temp_fd = open_netlink_socket(0);
        sync_fd = temp_fd;
    }
    if (sync_fd < 0) return -1;

    struct nl_link_info links[1] = {0};
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

    if (req.ifi.ifi_index == 0) {
        if (temp_fd >= 0) close(temp_fd);
        return -1;
    }

    int res = -1;
    pthread_mutex_lock(&g_nl_mutex);
    netlink_drain_socket(sync_fd);
    if (netlink_send_request(sync_fd, &req, req.nlh.nlmsg_len) == 0) {
        if (netlink_recv_all_with_timeout(sync_fd, req.nlh.nlmsg_seq,
                                          parser_link_sync, &ctx, 500) == 0) {
            if (ctx.count > 0) {
                *rx = links[0].rx_bytes;
                *tx = links[0].tx_bytes;
                res = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_nl_mutex);

    if (temp_fd >= 0) close(temp_fd);
    return res;
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

    vpn_bypass_sync(0);

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
        if (reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL) == 0) {
            g_xfrm_reactor = r;
            atomic_store(&g_xfrm_registered, 1);
        } else {
            LOG_ERROR("[XFRM] failed to register listener with reactor");
        }
    }
}

static int netlink_add_attr(struct nlmsghdr *n, size_t maxlen, int type, const void *data, size_t alen) {
    size_t rtalen = RTA_LENGTH(alen);
    size_t new_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rtalen);
    if (new_len > maxlen) {
        return -1;
    }
    struct rtattr *rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = rtalen;
    if (alen > 0 && data) {
        memcpy(RTA_DATA(rta), data, alen);
    }
    n->nlmsg_len = new_len;
    return 0;
}

static int netlink_rule_send(int cmd, int family, uint8_t dst_len, const uint8_t *addr, uint32_t table) {
    struct {
        struct nlmsghdr nlh;
        struct fib_rule_hdr frh;
        char buf[128];
    } req;
    memset(&req, 0, sizeof(req));

    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    req.nlh.nlmsg_type = (uint16_t)cmd;
    if (cmd == RTM_NEWRULE) {
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    } else {
        req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    }
    req.nlh.nlmsg_seq = atomic_fetch_add(&g_seq, 1);

    req.frh.family = (uint8_t)family;
    req.frh.dst_len = dst_len;
    req.frh.action = FR_ACT_TO_TBL;
    req.frh.table = table < 256 ? (uint8_t)table : RT_TABLE_UNSPEC;

    size_t addr_len = (family == AF_INET) ? 4 : 16;
    if (netlink_add_attr(&req.nlh, sizeof(req), FRA_DST, addr, addr_len) < 0) return -1;

    uint32_t pref = (uint32_t)g_bypass_pref;
    if (netlink_add_attr(&req.nlh, sizeof(req), FRA_PRIORITY, &pref, sizeof(pref)) < 0) return -1;

    if (netlink_add_attr(&req.nlh, sizeof(req), FRA_TABLE, &table, sizeof(table)) < 0) return -1;

    netlink_drain_socket(g_sync_fd);
    if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0) {
        LOG_WARN("Netlink: failed to send rule request (cmd %d): %s", cmd, strerror(errno));
        return -1;
    }
    if (netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq, NULL, NULL, 500) < 0) {
        return -1;
    }
    return 0;
}

struct route_find_ctx {
    uint32_t selected_table;
    char selected_ifname[IFNAMSIZ];
    int found_candidate;
    int found_wlan;
};

static int parser_route_find(struct nlmsghdr *h, void *userdata) {
    struct route_find_ctx *ctx = (struct route_find_ctx *)userdata;
    if (h->nlmsg_type != RTM_NEWROUTE) return 0;

    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(h);
    if ((rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6) || rtm->rtm_dst_len != 0) {
        return 0;
    }

    int len = RTM_PAYLOAD(h);
    uint32_t table = rtm->rtm_table;
    int oif = 0;
    int has_oif = 0;

    for (struct rtattr *rta = RTM_RTA(rtm); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == RTA_TABLE && RTA_PAYLOAD(rta) >= (int)sizeof(uint32_t)) {
            table = *(uint32_t *)RTA_DATA(rta);
        } else if (rta->rta_type == RTA_OIF && RTA_PAYLOAD(rta) >= (int)sizeof(int)) {
            oif = *(int *)RTA_DATA(rta);
            has_oif = 1;
        }
    }

    if (!has_oif) return 0;
    if (table < 256) return 0;

    char ifname[IFNAMSIZ] = {0};
    if (!if_indextoname((unsigned int)oif, ifname)) return 0;
    if (strcmp(ifname, "lo") == 0) return 0;
    if (is_proxy_interface(ifname)) return 0;

    /* shortcut: prefers wlan* when both Wi-Fi and mobile have defaults, upgrade to reading netd's default-network rule if that picks wrong */
    if (!ctx->found_candidate) {
        ctx->selected_table = table;
        safe_copy_ifname(ctx->selected_ifname, ifname, sizeof(ctx->selected_ifname));
        ctx->found_candidate = 1;
        if (strncmp(ifname, "wlan", 4) == 0) {
            ctx->found_wlan = 1;
        }
    } else if (!ctx->found_wlan && strncmp(ifname, "wlan", 4) == 0) {
        ctx->selected_table = table;
        safe_copy_ifname(ctx->selected_ifname, ifname, sizeof(ctx->selected_ifname));
        ctx->found_wlan = 1;
    }

    return 0;
}

#define MAX_DUMPED_OURS_RULES 64

typedef struct {
    int family;
    uint8_t dst_len;
    uint8_t addr[16];
    uint32_t table;
    int route_idx;
} dumped_ours_rule_t;

struct rule_dump_ctx {
    dumped_ours_rule_t rules[MAX_DUMPED_OURS_RULES];
    int count;
};

static int parser_rule_dump(struct nlmsghdr *h, void *userdata) {
    struct rule_dump_ctx *ctx = (struct rule_dump_ctx *)userdata;
    if (h->nlmsg_type != RTM_NEWRULE) return 0;

    struct fib_rule_hdr *frh = (struct fib_rule_hdr *)NLMSG_DATA(h);
    if (frh->family != AF_INET && frh->family != AF_INET6) return 0;
    if (frh->action != FR_ACT_TO_TBL) return 0;
    if (frh->src_len != 0 || frh->tos != 0) return 0;

    int rta_len = NLMSG_PAYLOAD(h, sizeof(struct fib_rule_hdr));
    struct rtattr *rta = (struct rtattr *)(((char *)NLMSG_DATA(h)) + NLMSG_ALIGN(sizeof(struct fib_rule_hdr)));

    uint32_t priority = 0;
    int has_priority = 0;
    uint32_t table = frh->table;
    const void *dst_addr = NULL;
    size_t dst_addr_len = 0;
    int has_other_selectors = 0;

    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case FRA_PRIORITY:
                if (RTA_PAYLOAD(rta) >= (int)sizeof(uint32_t)) {
                    priority = *(uint32_t *)RTA_DATA(rta);
                    has_priority = 1;
                }
                break;
            case FRA_TABLE:
                if (RTA_PAYLOAD(rta) >= (int)sizeof(uint32_t)) {
                    table = *(uint32_t *)RTA_DATA(rta);
                }
                break;
            case FRA_DST:
                dst_addr = RTA_DATA(rta);
                dst_addr_len = (size_t)RTA_PAYLOAD(rta);
                break;
            case FRA_SUPPRESS_PREFIXLEN:
            case FRA_SUPPRESS_IFGROUP:
                /* The kernel dumps these on every rule; 0xffffffff means "not set". */
                if (RTA_PAYLOAD(rta) < (int)sizeof(uint32_t) ||
                    *(uint32_t *)RTA_DATA(rta) != UINT32_MAX) {
                    has_other_selectors = 1;
                }
                break;
            case FRA_PROTOCOL:
            case FRA_PAD:
            case FRA_UNSPEC:
                break;
            default:
                has_other_selectors = 1;
                break;
        }
    }

    if (has_other_selectors) return 0;
    if (!has_priority || priority != (uint32_t)g_bypass_pref) return 0;
    if (!dst_addr) return 0;

    size_t expected_addr_len = (frh->family == AF_INET) ? 4 : 16;
    if (dst_addr_len < expected_addr_len) return 0;

    int matched_idx = -1;
    for (int i = 0; i < g_bypass_route_count; i++) {
        if (g_bypass_routes[i].family == frh->family &&
            g_bypass_routes[i].prefix_len == frh->dst_len &&
            memcmp(g_bypass_routes[i].addr, dst_addr, expected_addr_len) == 0) {
            matched_idx = i;
            break;
        }
    }

    if (matched_idx < 0) return 0;

    if (ctx->count < MAX_DUMPED_OURS_RULES) {
        dumped_ours_rule_t *r = &ctx->rules[ctx->count++];
        r->family = frh->family;
        r->dst_len = frh->dst_len;
        memcpy(r->addr, dst_addr, expected_addr_len);
        r->table = table;
        r->route_idx = matched_idx;
    }

    return 0;
}

static void vpn_bypass_sync(int ready) {
    if (g_bypass_route_count == 0) {
        return;
    }

    static int s_no_candidate_warned = 0;
    struct route_find_ctx r_ctx = {0};
    uint32_t selected_table = 0;
    char selected_ifname[IFNAMSIZ] = {0};

    pthread_mutex_lock(&g_nl_mutex);
    if (g_sync_fd < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        return;
    }

    if (ready) {
        struct {
            struct nlmsghdr nlh;
            struct rtmsg rtm;
        } req = {
            .nlh = {
                .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg)),
                .nlmsg_type = RTM_GETROUTE,
                .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
                .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
            },
            .rtm = {
                .rtm_family = AF_INET,
                .rtm_table = RT_TABLE_UNSPEC
            }
        };

        netlink_drain_socket(g_sync_fd);
        /* A failed dump must not look like "no physical table": that would delete working rules. */
        if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0 ||
            netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                          parser_route_find, &r_ctx, 500) < 0) {
            LOG_WARN("VPN bypass: route dump failed, leaving rules unchanged");
            pthread_mutex_unlock(&g_nl_mutex);
            return;
        }

        if (!r_ctx.found_candidate) {
            /* Try IPv6 default route if IPv4 didn't provide one (e.g. pure IPv6 network) */
            req.nlh.nlmsg_seq = atomic_fetch_add(&g_seq, 1);
            req.rtm.rtm_family = AF_INET6;
            if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0 ||
                netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                              parser_route_find, &r_ctx, 500) < 0) {
                LOG_WARN("VPN bypass: IPv6 route dump failed, leaving rules unchanged");
                pthread_mutex_unlock(&g_nl_mutex);
                return;
            }
        }

        if (!r_ctx.found_candidate) {
            if (!s_no_candidate_warned) {
                LOG_WARN("VPN bypass: physical default route table not found");
                s_no_candidate_warned = 1;
            }
            ready = 0;
        } else {
            s_no_candidate_warned = 0;
            selected_table = r_ctx.selected_table;
            safe_copy_ifname(selected_ifname, r_ctx.selected_ifname, sizeof(selected_ifname));
        }
    } else {
        s_no_candidate_warned = 0;
    }

    struct rule_dump_ctx rule_ctx = {0};
    int families[2] = { AF_INET, AF_INET6 };
    for (int f = 0; f < 2; f++) {
        struct {
            struct nlmsghdr nlh;
            struct fib_rule_hdr frh;
        } req = {
            .nlh = {
                .nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr)),
                .nlmsg_type = RTM_GETRULE,
                .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
                .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
            },
            .frh = {
                .family = (uint8_t)families[f]
            }
        };

        netlink_drain_socket(g_sync_fd);
        if (netlink_send_request(g_sync_fd, &req, req.nlh.nlmsg_len) < 0 ||
            netlink_recv_all_with_timeout(g_sync_fd, req.nlh.nlmsg_seq,
                                          parser_rule_dump, &rule_ctx, 500) < 0) {
            LOG_WARN("VPN bypass: rule dump failed, leaving rules unchanged");
            pthread_mutex_unlock(&g_nl_mutex);
            return;
        }
    }

    int rules_added = 0;
    int rules_removed = 0;

    if (ready) {
        for (int i = 0; i < g_bypass_route_count; i++) {
            int has_table_t = 0;
            for (int j = 0; j < rule_ctx.count; j++) {
                if (rule_ctx.rules[j].route_idx == i) {
                    if (rule_ctx.rules[j].table == selected_table) {
                        if (!has_table_t) {
                            has_table_t = 1;
                        } else {
                            if (netlink_rule_send(RTM_DELRULE,
                                                  rule_ctx.rules[j].family,
                                                  rule_ctx.rules[j].dst_len,
                                                  rule_ctx.rules[j].addr,
                                                  rule_ctx.rules[j].table) == 0) {
                                rules_removed++;
                            }
                        }
                    } else {
                        if (netlink_rule_send(RTM_DELRULE,
                                              rule_ctx.rules[j].family,
                                              rule_ctx.rules[j].dst_len,
                                              rule_ctx.rules[j].addr,
                                              rule_ctx.rules[j].table) == 0) {
                            rules_removed++;
                        }
                    }
                }
            }
            if (!has_table_t) {
                if (netlink_rule_send(RTM_NEWRULE,
                                      g_bypass_routes[i].family,
                                      g_bypass_routes[i].prefix_len,
                                      g_bypass_routes[i].addr,
                                      selected_table) == 0) {
                    rules_added++;
                }
            }
        }
    } else {
        for (int j = 0; j < rule_ctx.count; j++) {
            if (netlink_rule_send(RTM_DELRULE,
                                  rule_ctx.rules[j].family,
                                  rule_ctx.rules[j].dst_len,
                                  rule_ctx.rules[j].addr,
                                  rule_ctx.rules[j].table) == 0) {
                rules_removed++;
            }
        }
    }

    pthread_mutex_unlock(&g_nl_mutex);

    if (rules_added > 0) {
        LOG_INFO("VPN bypass: added %d rule(s) to table %u (%s)",
                 rules_added, selected_table, selected_ifname);
    }
    if (rules_removed > 0) {
        LOG_INFO("VPN bypass: removed %d rule(s)", rules_removed);
    }
}

static void debounce_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;

    pthread_mutex_lock(&g_debounce_lock);
    g_debounce_timer = NULL;
    pthread_mutex_unlock(&g_debounce_lock);

    char vpn[IFNAMSIZ] = {0};
    int has_vpn = (netlink_get_active_vpn(vpn, sizeof(vpn)) == 0 && vpn[0]);
    if (has_vpn) {
        atpd_vpn_state_transition(VPN_STATE_READY, 0, vpn);
    } else {
        atpd_vpn_state_transition(VPN_STATE_IDLE, 0, NULL);
    }
    int ready = (has_vpn && strncmp(vpn, "ipsec", 5) == 0);
    vpn_bypass_sync(ready);
}

static void trigger_network_refresh_delay(reactor_t *r, int delay_ms) {
    if (!r) return;

    pthread_mutex_lock(&g_debounce_lock);
    if (g_debounce_timer) {
        reactor_cancel_timer(r, g_debounce_timer);
        g_debounce_timer = NULL;
    }
    g_debounce_timer = reactor_add_timer(r, delay_ms, 0, debounce_timer_cb, NULL);
    if (!g_debounce_timer) {
        LOG_ERROR("Netlink: failed to schedule refresh timer");
    }
    pthread_mutex_unlock(&g_debounce_lock);
}

static void trigger_network_refresh(reactor_t *r) {
    trigger_network_refresh_delay(r, NETLINK_DEBOUNCE_MS);
}

void netlink_refresh_state(reactor_t *r) {
    /* Match the shell sentinel's initial/transition settle window. */
    trigger_network_refresh_delay(r, NETLINK_VPN_SETTLE_MS);
}

void netlink_handle_event(reactor_t *r, int fd, uint32_t events, void *data) {
    (void)r;
    (void)events;
    (void)data;
    char buf[NL_BUF_SIZE];
    ssize_t len = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0) return;

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK ||
            h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR ||
            h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_DELROUTE ||
            h->nlmsg_type == RTM_NEWRULE || h->nlmsg_type == RTM_DELRULE) {
            trigger_network_refresh(g_debounce_reactor);
        }
    }
}
