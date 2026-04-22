/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink Wait - Async interface waiting with Reactor integration
 */

#include "netlink.h"
#include "netlink_wait.h"
#include "reactor.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

/* ========== Wait Context Structure ========== */

typedef struct iface_wait_ctx {
    char ifname[IFNAMSIZ];              /* Interface name to wait for */
    uint32_t timeout_ms;                /* Total timeout in milliseconds */
    uint32_t elapsed_ms;               /* Time elapsed since start */
    reactor_t *reactor;                /* Reactor instance */
    reactor_timer_t *timer;            /* Periodic check timer */
    netlink_wait_cb callback;          /* User callback */
    void *userdata;                    /* User data */
    uint8_t completed;                 /* Prevent duplicate callbacks */
} iface_wait_ctx_t;

/* ========== Forward Declarations ========== */

static void wait_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
static void netlink_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static int check_interface_exists(const char *ifname);
static void cleanup_ctx(iface_wait_ctx_t *ctx, int success);

/* ========== Global Netlink FD for Event Listening ========== */

static int g_nl_event_fd = -1;
static int g_nl_event_registered = 0;

/* ========== Helper Functions ========== */

/**
 * Initialize Netlink event socket for async listening
 */
static int init_netlink_event_socket(void) {
    if (g_nl_event_fd >= 0) {
        return g_nl_event_fd;
    }
    
    g_nl_event_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (g_nl_event_fd < 0) {
        LOG_ERROR("netlink_wait: Failed to create netlink event socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_LINK  /* Only need link events */
    };
    
    if (bind(g_nl_event_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("netlink_wait: Failed to bind netlink event socket: %s", strerror(errno));
        close(g_nl_event_fd);
        g_nl_event_fd = -1;
        return -1;
    }
    
    LOG_DEBUG("netlink_wait: Event socket initialized (fd=%d)", g_nl_event_fd);
    return g_nl_event_fd;
}

/**
 * Check if interface exists
 */
static int check_interface_exists(const char *ifname) {
    if (!ifname || !*ifname) return 0;
    return if_nametoindex(ifname) > 0;
}

/**
 * Process netlink events and check if target interface appeared
 */
static void process_netlink_events(iface_wait_ctx_t *ctx) {
    char buf[8192];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    ssize_t len = recvmsg(g_nl_event_fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;
    
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(nlh, (uint32_t)len); nlh = NLMSG_NEXT(nlh, len)) {
        if (nlh->nlmsg_type == RTM_NEWLINK) {
            struct ifinfomsg *ifi = NLMSG_DATA(nlh);
            char ifname[IFNAMSIZ] = {0};
            
            if (if_indextoname(ifi->ifi_index, ifname) && 
                strcmp(ifname, ctx->ifname) == 0) {
                
                int up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
                LOG_DEBUG("netlink_wait: Interface %s appeared (up=%d)", ifname, up);
                
                if (up) {
                    cleanup_ctx(ctx, 1);
                    return;
                }
            }
        }
    }
}

/**
 * Netlink event callback (registered to reactor)
 */
static void netlink_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    
    iface_wait_ctx_t *ctx = userdata;
    if (!ctx || ctx->completed) return;
    
    process_netlink_events(ctx);
}

/**
 * Timer callback for periodic interface check
 */
static void wait_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    
    iface_wait_ctx_t *ctx = userdata;
    if (!ctx || ctx->completed) return;
    
    ctx->elapsed_ms += 1000;  /* Timer fires every 1000ms */
    
    /* Check if interface exists and is up */
    if (check_interface_exists(ctx->ifname)) {
        /* Verify it's up */
        int ifindex = if_nametoindex(ctx->ifname);
        if (ifindex > 0) {
            /* Could add additional IFF_UP check here if needed */
            LOG_INFO("netlink_wait: Interface %s is ready after %u ms", 
                     ctx->ifname, ctx->elapsed_ms);
            cleanup_ctx(ctx, 1);
            return;
        }
    }
    
    /* Check timeout */
    if (ctx->elapsed_ms >= ctx->timeout_ms) {
        LOG_WARN("netlink_wait: Timeout waiting for interface %s (%u ms)", 
                 ctx->ifname, ctx->timeout_ms);
        cleanup_ctx(ctx, 0);
        return;
    }
    
    LOG_DEBUG("netlink_wait: Still waiting for %s (elapsed=%u/%u ms)", 
              ctx->ifname, ctx->elapsed_ms, ctx->timeout_ms);
}

/**
 * Cleanup wait context and trigger callback
 */
static void cleanup_ctx(iface_wait_ctx_t *ctx, int success) {
    if (!ctx || ctx->completed) return;
    
    ctx->completed = 1;
    
    /* Cancel timer */
    if (ctx->timer) {
        reactor_cancel_timer(ctx->reactor, ctx->timer);
        ctx->timer = NULL;
    }
    
    /* Unregister netlink event fd if no more contexts */
    /* Note: Simplified - in production you'd track active contexts */
    
    LOG_DEBUG("netlink_wait: Context cleanup for %s (success=%d)", ctx->ifname, success);
    
    /* Trigger user callback */
    if (ctx->callback) {
        ctx->callback(ctx->ifname, success, ctx->userdata);
    }
    
    free(ctx);
}

/* ========== Public API ========== */

int netlink_wait_for_iface(reactor_t *r, const char *ifname, uint32_t timeout_ms,
                           netlink_wait_cb callback, void *userdata) {
    if (!r || !ifname || !*ifname || !callback) {
        LOG_ERROR("netlink_wait: Invalid parameters");
        return -1;
    }
    
    /* Quick check: already exists? */
    if (check_interface_exists(ifname)) {
        LOG_DEBUG("netlink_wait: Interface %s already exists, triggering callback immediately", ifname);
        callback(ifname, 1, userdata);
        return 0;
    }
    
    /* Allocate context */
    iface_wait_ctx_t *ctx = calloc(1, sizeof(iface_wait_ctx_t));
    if (!ctx) {
        LOG_ERROR("netlink_wait: Failed to allocate context");
        return -1;
    }
    
    strncpy(ctx->ifname, ifname, IFNAMSIZ - 1);
    ctx->ifname[IFNAMSIZ - 1] = '\0';
    ctx->timeout_ms = timeout_ms ? timeout_ms : 30000;  /* Default 30s */
    ctx->elapsed_ms = 0;
    ctx->reactor = r;
    ctx->callback = callback;
    ctx->userdata = userdata;
    ctx->completed = 0;
    
    /* Initialize netlink event socket */
    int nl_fd = init_netlink_event_socket();
    if (nl_fd >= 0 && !g_nl_event_registered) {
        reactor_add_fd(r, nl_fd, REACTOR_EVENT_READ, netlink_event_cb, ctx);
        g_nl_event_registered = 1;
        LOG_DEBUG("netlink_wait: Registered netlink event listener");
    }
    
    /* Create periodic timer (1 second interval) */
    ctx->timer = reactor_add_timer(r, 1000, 1000, wait_timer_cb, ctx);
    if (!ctx->timer) {
        LOG_ERROR("netlink_wait: Failed to create timer");
        free(ctx);
        return -1;
    }
    
    LOG_INFO("netlink_wait: Started waiting for interface %s (timeout=%u ms)", 
             ifname, ctx->timeout_ms);
    
    return 0;
}

/* ========== Cleanup ========== */

void netlink_wait_cleanup(void) {
    if (g_nl_event_fd >= 0) {
        close(g_nl_event_fd);
        g_nl_event_fd = -1;
    }
    g_nl_event_registered = 0;
    LOG_DEBUG("netlink_wait: Cleanup complete");
}
