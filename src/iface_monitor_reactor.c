/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 * 
 * Interface Monitor - Reactor Integration Layer
 */

#include "iface_monitor.h"
#include "reactor.h"
#include "netlink.h"
#include "logger.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ========== Reactor Monitor Context ========== */

typedef struct {
    iface_monitor_t base;
    reactor_t *reactor;
    reactor_timer_t *vpn_check_timer;
    int nl_fd;
    uint8_t owned_reactor;  /* Whether we created the reactor */
} reactor_monitor_ctx_t;

/* ========== Forward Declarations ========== */

static void nl_event_callback(reactor_t *r, int fd, uint32_t events, void *userdata);
static void vpn_check_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
static void process_netlink_message(reactor_monitor_ctx_t *ctx);

/* ========== Helper Functions ========== */

static void check_vpn_status_reactor(reactor_monitor_ctx_t *ctx) {
    int vpn_enabled = nl_vpn_detect();
    
    if (vpn_enabled != ctx->base.vpn_enabled) {
        ctx->base.vpn_enabled = vpn_enabled;
        if (ctx->base.callback) {
            iface_event_t event = vpn_enabled ? 
                IFACE_EVENT_VPN_CONNECTED : IFACE_EVENT_VPN_DISCONNECTED;
            ctx->base.callback(NULL, event, ctx->base.userdata);
        }
        LOG_INFO("VPN status changed: %s", vpn_enabled ? "connected" : "disconnected");
    }
    
    char vpn_iface[IFNAMSIZ];
    if (nl_link_get_vpn_interface(vpn_iface, sizeof(vpn_iface)) == 0) {
        if (strcmp(vpn_iface, ctx->base.current_vpn_iface) != 0) {
            if (ctx->base.current_vpn_iface[0] != '\0') {
                LOG_INFO("VPN interface changed: %s -> %s",
                         ctx->base.current_vpn_iface, vpn_iface);
            }
            strncpy(ctx->base.current_vpn_iface, vpn_iface, IFNAMSIZ - 1);
            ctx->base.current_vpn_iface[IFNAMSIZ - 1] = '\0';
            
            if (ctx->base.callback) {
                ctx->base.callback(vpn_iface, IFACE_EVENT_ADDED, ctx->base.userdata);
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
    
    for (struct nlmsghdr *nlh = (struct nlmsghdr *)buf; 
         NLMSG_OK(nlh, (uint32_t)len); 
         nlh = NLMSG_NEXT(nlh, len)) {
        
        char ifname[IFNAMSIZ] = {0};
        int event_type = -1;
        
        if (nlh->nlmsg_type == RTM_NEWLINK) {
            struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
            if_indextoname(ifi->ifi_index, ifname);
            
            int up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
            event_type = up ? IFACE_EVENT_ADDED : IFACE_EVENT_REMOVED;
            
        } else if (nlh->nlmsg_type == RTM_DELLINK) {
            struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
            if_indextoname(ifi->ifi_index, ifname);
            event_type = IFACE_EVENT_REMOVED;
            
        } else if (nlh->nlmsg_type == RTM_NEWADDR) {
            struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
            if_indextoname(ifa->ifa_index, ifname);
            event_type = IFACE_EVENT_ADDR_ADDED;
            
        } else if (nlh->nlmsg_type == RTM_DELADDR) {
            struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
            if_indextoname(ifa->ifa_index, ifname);
            event_type = IFACE_EVENT_ADDR_REMOVED;
        }
        
        if (event_type != -1 && ifname[0] != '\0' && ctx->base.callback) {
            ctx->base.callback(ifname, event_type, ctx->base.userdata);
        }
    }
    
    /* Check VPN status after processing netlink events */
    check_vpn_status_reactor(ctx);
}

/* ========== Reactor Callbacks ========== */

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
    
    check_vpn_status_reactor(ctx);
}

/* ========== Public API Implementation ========== */

int iface_monitor_init_reactor(iface_monitor_t *monitor, 
                                iface_callback_t callback, 
                                void *userdata,
                                reactor_t *existing_reactor) {
    if (!monitor) return -1;
    
    reactor_monitor_ctx_t *ctx = (reactor_monitor_ctx_t *)monitor;
    memset(ctx, 0, sizeof(reactor_monitor_ctx_t));
    
    /* Create or use existing reactor */
    if (existing_reactor) {
        ctx->reactor = existing_reactor;
        ctx->owned_reactor = 0;
    } else {
        ctx->reactor = reactor_create();
        if (!ctx->reactor) {
            LOG_ERROR("Failed to create reactor");
            return -1;
        }
        ctx->owned_reactor = 1;
    }
    
    /* Create netlink socket */
    ctx->nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (ctx->nl_fd < 0) {
        LOG_ERROR("Failed to create netlink socket: %s", strerror(errno));
        if (ctx->owned_reactor) reactor_destroy(ctx->reactor);
        return -1;
    }
    
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
    };
    
    if (bind(ctx->nl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind netlink socket: %s", strerror(errno));
        close(ctx->nl_fd);
        if (ctx->owned_reactor) reactor_destroy(ctx->reactor);
        return -1;
    }
    
    /* Register netlink fd with reactor */
    if (reactor_add_fd(ctx->reactor, ctx->nl_fd, REACTOR_EVENT_READ, 
                       nl_event_callback, ctx) < 0) {
        LOG_ERROR("Failed to register netlink fd with reactor");
        close(ctx->nl_fd);
        if (ctx->owned_reactor) reactor_destroy(ctx->reactor);
        return -1;
    }
    
    /* Set up callbacks */
    ctx->base.callback = callback;
    ctx->base.userdata = userdata;
    ctx->base.sock_fd = ctx->nl_fd;
    ctx->base.running = 0;
    ctx->base.vpn_enabled = 0;
    ctx->base.current_vpn_iface[0] = '\0';
    
    /* Watch for termination signals */
    reactor_watch_signal(ctx->reactor, SIGINT);
    reactor_watch_signal(ctx->reactor, SIGTERM);
    
    LOG_INFO("Interface monitor initialized with reactor");
    return 0;
}

int iface_monitor_start_reactor(iface_monitor_t *monitor) {
    if (!monitor) return -1;
    
    reactor_monitor_ctx_t *ctx = (reactor_monitor_ctx_t *)monitor;
    
    ctx->base.running = 1;
    
    /* Initial state dump */
    nl_send_link_dump(ctx->nl_fd);
    nl_send_addr_dump(ctx->nl_fd, AF_INET);
    nl_send_addr_dump(ctx->nl_fd, AF_INET6);
    
    /* Initial VPN check */
    check_vpn_status_reactor(ctx);
    
    /* Set up periodic VPN check (every 5 seconds) */
    ctx->vpn_check_timer = reactor_add_timer(ctx->reactor, 
                                              5000,  /* initial delay 5s */
                                              5000,  /* interval 5s */
                                              vpn_check_timer_cb, 
                                              ctx);
    
    LOG_INFO("Interface monitor started (reactor mode)");
    return 0;
}

int iface_monitor_run_reactor(iface_monitor_t *monitor) {
    if (!monitor) return -1;
    
    reactor_monitor_ctx_t *ctx = (reactor_monitor_ctx_t *)monitor;
    
    if (!ctx->owned_reactor) {
        LOG_WARN("Cannot run reactor owned by caller");
        return -1;
    }
    
    return reactor_run(ctx->reactor);
}

void iface_monitor_stop_reactor(iface_monitor_t *monitor) {
    if (!monitor) return;
    
    reactor_monitor_ctx_t *ctx = (reactor_monitor_ctx_t *)monitor;
    
    ctx->base.running = 0;
    
    if (ctx->vpn_check_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->vpn_check_timer);
        ctx->vpn_check_timer = NULL;
    }
    
    LOG_INFO("Interface monitor stopped (reactor mode)");
}

void iface_monitor_cleanup_reactor(iface_monitor_t *monitor) {
    if (!monitor) return;
    
    reactor_monitor_ctx_t *ctx = (reactor_monitor_ctx_t *)monitor;
    
    iface_monitor_stop_reactor(monitor);
    
    if (ctx->nl_fd >= 0) {
        reactor_remove_fd(ctx->reactor, ctx->nl_fd);
        close(ctx->nl_fd);
        ctx->nl_fd = -1;
    }
    
    if (ctx->owned_reactor && ctx->reactor) {
        reactor_destroy(ctx->reactor);
        ctx->reactor = NULL;
    }
    
    LOG_INFO("Interface monitor cleaned up (reactor mode)");
}

reactor_t* iface_monitor_get_reactor(iface_monitor_t *monitor) {
    if (!monitor) return NULL;
    reactor_monitor_ctx_t *ctx = (reactor_monitor_ctx_t *)monitor;
    return ctx->reactor;
}

/* ========== Size Verification ========== */

/* Ensure reactor_monitor_ctx_t fits in iface_monitor_t's reserved space */
_Static_assert(sizeof(reactor_monitor_ctx_t) <= sizeof(iface_monitor_t),
               "reactor_monitor_ctx_t too large for iface_monitor_t");
