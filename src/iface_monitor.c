/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Interface monitor - Reactor-driven netlink
 */

#include "iface_monitor.h"
#include "logger.h"
#include "netlink.h"
#include "utils.h"
#include "reactor.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>

typedef struct {
    iface_monitor_t *base;
    reactor_t *reactor;
    reactor_timer_t *vpn_check_timer;
    int nl_fd;
    uint8_t owned_reactor;
} reactor_monitor_ctx_t;

static void check_vpn_status(reactor_monitor_ctx_t *ctx) {
    if (!ctx || !ctx->base) return;
    
    int vpn_enabled = nl_vpn_detect();
    
    if (vpn_enabled != ctx->base->vpn_enabled) {
        ctx->base->vpn_enabled = vpn_enabled;
        if (ctx->base->callback) {
            iface_event_t event = vpn_enabled ? 
                IFACE_EVENT_VPN_CONNECTED : IFACE_EVENT_VPN_DISCONNECTED;
            ctx->base->callback(NULL, event, ctx->base->userdata);
        }
    }
    
    char vpn_iface[IFNAMSIZ];
    if (nl_link_get_vpn_interface(vpn_iface, sizeof(vpn_iface)) == 0) {
        if (strcmp(vpn_iface, ctx->base->current_vpn_iface) != 0) {
            strncpy(ctx->base->current_vpn_iface, vpn_iface, IFNAMSIZ - 1);
            ctx->base->current_vpn_iface[IFNAMSIZ - 1] = '\0';
            
            if (ctx->base->callback) {
                ctx->base->callback(vpn_iface, IFACE_EVENT_ADDED, ctx->base->userdata);
            }
        }
    }
}

static void process_netlink_message(reactor_monitor_ctx_t *ctx) {
    char buf[8192];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    ssize_t len = recvmsg(ctx->nl_fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;
    
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(nlh, (uint32_t)len); nlh = NLMSG_NEXT(nlh, len)) {
        char ifname[IFNAMSIZ] = {0};
        int event_type = -1;
        
        if (nlh->nlmsg_type == RTM_NEWLINK) {
            struct ifinfomsg *ifi = NLMSG_DATA(nlh);
            if_indextoname(ifi->ifi_index, ifname);
            int up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
            event_type = up ? IFACE_EVENT_UP : IFACE_EVENT_DOWN;
        } else if (nlh->nlmsg_type == RTM_DELLINK) {
            struct ifinfomsg *ifi = NLMSG_DATA(nlh);
            if_indextoname(ifi->ifi_index, ifname);
            event_type = IFACE_EVENT_REMOVED;
        } else if (nlh->nlmsg_type == RTM_NEWADDR) {
            struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
            if_indextoname(ifa->ifa_index, ifname);
            event_type = IFACE_EVENT_ADDR_ADDED;
        } else if (nlh->nlmsg_type == RTM_DELADDR) {
            struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
            if_indextoname(ifa->ifa_index, ifname);
            event_type = IFACE_EVENT_ADDR_REMOVED;
        }
        
        if (event_type != -1 && ifname[0] != '\0' && ctx->base->callback) {
            ctx->base->callback(ifname, event_type, ctx->base->userdata);
        }
    }
    
    check_vpn_status(ctx);
}

static void nl_event_callback(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    reactor_monitor_ctx_t *ctx = userdata;
    if (!ctx || fd != ctx->nl_fd) return;
    process_netlink_message(ctx);
}

static void vpn_check_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    reactor_monitor_ctx_t *ctx = userdata;
    if (!ctx) return;
    check_vpn_status(ctx);
}

int iface_monitor_init(iface_monitor_t *monitor, iface_callback_t callback, void *userdata) {
    memset(monitor, 0, sizeof(iface_monitor_t));
    
    monitor->callback = callback;
    monitor->userdata = userdata;
    monitor->running = 0;
    monitor->vpn_enabled = 0;
    monitor->current_vpn_iface[0] = '\0';
    
    LOG_DEBUG("Interface monitor initialized");
    return 0;
}

int iface_monitor_start(iface_monitor_t *monitor) {
    if (!monitor || !monitor->internal) return -1;
    reactor_monitor_ctx_t *ctx = monitor->internal;
    
    monitor->running = 1;
    
    check_vpn_status(ctx);
    
    ctx->vpn_check_timer = reactor_add_timer(ctx->reactor, 5000, 5000, vpn_check_timer_cb, ctx);
    
    return 0;
}

int iface_monitor_stop(iface_monitor_t *monitor) {
    if (!monitor || !monitor->internal) return -1;
    reactor_monitor_ctx_t *ctx = monitor->internal;

    monitor->running = 0;

    if (ctx->vpn_check_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->vpn_check_timer);
        ctx->vpn_check_timer = NULL;
    }

    return 0;
}

void iface_monitor_cleanup(iface_monitor_t *monitor) {
    if (!monitor || !monitor->internal) return;
    reactor_monitor_ctx_t *ctx = monitor->internal;
    
    iface_monitor_stop(monitor);
    
    if (ctx->nl_fd >= 0) {
        reactor_remove_fd(ctx->reactor, ctx->nl_fd);
        close(ctx->nl_fd);
    }
    
    if (ctx->owned_reactor && ctx->reactor) {
        reactor_destroy(ctx->reactor);
    }
    
    free(ctx);
    monitor->internal = NULL;
}

int iface_monitor_poll(iface_monitor_t *monitor, int timeout_ms) {
    (void)monitor;
    (void)timeout_ms;
    return 0;
}

int iface_monitor_init_reactor(iface_monitor_t *monitor, 
                                iface_callback_t callback, 
                                void *userdata,
                                reactor_t *existing_reactor) {
    if (!monitor) return -1;
    
    reactor_monitor_ctx_t *ctx = calloc(1, sizeof(reactor_monitor_ctx_t));
    if (!ctx) return -1;
    
    monitor->internal = ctx;
    ctx->base = monitor;
    
    if (existing_reactor) {
        ctx->reactor = existing_reactor;
        ctx->owned_reactor = 0;
    } else {
        ctx->reactor = reactor_create();
        if (!ctx->reactor) {
            free(ctx);
            return -1;
        }
        ctx->owned_reactor = 1;
    }
    
    ctx->nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (ctx->nl_fd < 0) {
        if (ctx->owned_reactor) reactor_destroy(ctx->reactor);
        free(ctx);
        return -1;
    }
    
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
    };
    
    if (bind(ctx->nl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ctx->nl_fd);
        if (ctx->owned_reactor) reactor_destroy(ctx->reactor);
        free(ctx);
        return -1;
    }
    
    reactor_add_fd(ctx->reactor, ctx->nl_fd, REACTOR_EVENT_READ, nl_event_callback, ctx);
    
    monitor->callback = callback;
    monitor->userdata = userdata;
    monitor->sock_fd = ctx->nl_fd;
    monitor->running = 0;
    monitor->vpn_enabled = 0;
    monitor->current_vpn_iface[0] = '\0';
    
    reactor_watch_signal(ctx->reactor, SIGINT);
    reactor_watch_signal(ctx->reactor, SIGTERM);
    
    return 0;
}

int iface_monitor_start_reactor(iface_monitor_t *monitor) {
    return iface_monitor_start(monitor);
}

int iface_monitor_run_reactor(iface_monitor_t *monitor) {
    if (!monitor || !monitor->internal) return -1;
    reactor_monitor_ctx_t *ctx = monitor->internal;
    
    if (!ctx->owned_reactor) return -1;
    
    return reactor_run(ctx->reactor);
}

void iface_monitor_stop_reactor(iface_monitor_t *monitor) {
    iface_monitor_stop(monitor);
}

void iface_monitor_cleanup_reactor(iface_monitor_t *monitor) {
    iface_monitor_cleanup(monitor);
}

reactor_t* iface_monitor_get_reactor(iface_monitor_t *monitor) {
    if (!monitor || !monitor->internal) return NULL;
    reactor_monitor_ctx_t *ctx = monitor->internal;
    return ctx->reactor;
}
