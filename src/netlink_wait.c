#include "atpd_global.h"
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink Wait - Async interface waiting with Reactor integration
 * Supports parallel waiting for multiple interfaces.
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
#include <pthread.h>

/* ========== Wait Context Structure ========== */

typedef struct iface_wait_ctx {
    char ifname[IFNAMSIZ];
    uint32_t timeout_ms;
    uint32_t elapsed_ms;
    reactor_t *reactor;
    reactor_timer_t *timer;
    netlink_wait_cb callback;
    void *userdata;
    uint8_t completed;
    struct iface_wait_ctx *next;
} iface_wait_ctx_t;

/* ========== Global Management ========== */

static iface_wait_ctx_t *g_ctx_list = NULL;
static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_nl_event_fd = -1;
static int g_nl_event_registered = 0;
static reactor_t *g_reactor = NULL;

/* ========== Forward Declarations ========== */

static void wait_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
static void netlink_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata);
static int check_interface_exists(const char *ifname);
static void fire_callbacks_and_free(iface_wait_ctx_t *head, int success);

/* ========== Helper Functions ========== */

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
        .nl_groups = RTMGRP_LINK
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

static int check_interface_exists(const char *ifname) {
    if (!ifname || !*ifname) return 0;
    unsigned int idx = if_nametoindex(ifname);
    return idx > 0;
}

static void fire_callbacks_and_free(iface_wait_ctx_t *head, int success) {
    while (head) {
        iface_wait_ctx_t *next = head->next;
        
        if (head->timer) {
            reactor_cancel_timer(head->reactor, head->timer);
            head->timer = NULL;
        }

        LOG_DEBUG("netlink_wait: Context cleanup for %s (success=%d)", head->ifname, success);
        
        if (head->callback) {
            head->callback(head->ifname, success, head->userdata);
        }
        
        free(head);
        head = next;
    }
}

static void process_netlink_events(void) {
    char buf[4096];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    iface_wait_ctx_t *completed_list = NULL;

    while (1) {
        ssize_t len = recvmsg(g_nl_event_fd, &msg, MSG_DONTWAIT);
        if (len <= 0) {
            if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("netlink_wait: recvmsg error: %s", strerror(errno));
            }
            break;
        }
        
        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(nlh, (uint32_t)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type != RTM_NEWLINK) continue;
            
            struct ifinfomsg *ifi = NLMSG_DATA(nlh);
            char ifname[IFNAMSIZ];
            
            if (!if_indextoname(ifi->ifi_index, ifname)) {
                continue;
            }
            
            int up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
            if (!up) continue;
            
            LOG_DEBUG("netlink_wait: RTM_NEWLINK event for %s (up=%d)", ifname, up);
            
            pthread_mutex_lock(&g_list_mutex);
            
            iface_wait_ctx_t **pp = &g_ctx_list;
            while (*pp) {
                iface_wait_ctx_t *ctx = *pp;
                if (!ctx->completed && strcmp(ctx->ifname, ifname) == 0) {
                    LOG_INFO("netlink_wait: Interface %s appeared via netlink event", ifname);
                    ctx->completed = 1;
                    *pp = ctx->next; // Remove from global list
                    
                    // Add to local completed list
                    ctx->next = completed_list;
                    completed_list = ctx;
                } else {
                    pp = &ctx->next;
                }
            }
            
            pthread_mutex_unlock(&g_list_mutex);
        }
    }

    // Fire callbacks safely outside the lock
    if (completed_list) {
        fire_callbacks_and_free(completed_list, 1);
    }
}

static void netlink_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)fd;
    (void)events;
    (void)userdata;
    process_netlink_events();
}

static void wait_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    
    iface_wait_ctx_t *target_ctx = userdata;
    if (!target_ctx || target_ctx->completed) return;

    iface_wait_ctx_t *extracted_ctx = NULL;
    int success = 0;

    pthread_mutex_lock(&g_list_mutex);
    iface_wait_ctx_t **pp = &g_ctx_list;
    while (*pp) {
        if (*pp == target_ctx) {
            target_ctx->elapsed_ms += 1000;
            
            if (check_interface_exists(target_ctx->ifname)) {
                LOG_INFO("netlink_wait: Interface %s is ready after %u ms", 
                         target_ctx->ifname, target_ctx->elapsed_ms);
                success = 1;
            } else if (target_ctx->elapsed_ms >= target_ctx->timeout_ms) {
                LOG_WARN("netlink_wait: Timeout waiting for interface %s (%u ms)", 
                         target_ctx->ifname, target_ctx->timeout_ms);
                success = 0;
            } else {
                LOG_DEBUG("netlink_wait: Still waiting for %s (elapsed=%u/%u ms)", 
                          target_ctx->ifname, target_ctx->elapsed_ms, target_ctx->timeout_ms);
                pp = &(*pp)->next;
                continue; // Do not extract, keep waiting
            }

            // Extract context
            target_ctx->completed = 1;
            *pp = target_ctx->next;
            extracted_ctx = target_ctx;
            extracted_ctx->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_list_mutex);

    // Fire callback outside the lock
    if (extracted_ctx) {
        fire_callbacks_and_free(extracted_ctx, success);
    }
}

/* ========== Public API ========== */

int netlink_wait_for_iface(reactor_t *r, const char *ifname, uint32_t timeout_ms,
                           netlink_wait_cb callback, void *userdata) {
    if (!r || !ifname || !*ifname || !callback) {
        LOG_ERROR("netlink_wait: Invalid parameters");
        return -1;
    }
    
    g_reactor = r;
    
    if (check_interface_exists(ifname)) {
        LOG_DEBUG("netlink_wait: Interface %s already exists, triggering callback immediately", ifname);
        callback(ifname, 1, userdata);
        return 0;
    }
    
    iface_wait_ctx_t *ctx = calloc(1, sizeof(iface_wait_ctx_t));
    if (!ctx) {
        LOG_ERROR("netlink_wait: Failed to allocate context");
        return -1;
    }
    
    snprintf(ctx->ifname, sizeof(ctx->ifname), "%s", ifname);
    ctx->timeout_ms = timeout_ms ? timeout_ms : 30000;
    ctx->elapsed_ms = 0;
    ctx->reactor = r;
    ctx->callback = callback;
    ctx->userdata = userdata;
    ctx->completed = 0;
    ctx->next = NULL;
    
    int nl_fd = init_netlink_event_socket();
    if (nl_fd >= 0 && !g_nl_event_registered) {
        reactor_add_fd(r, nl_fd, REACTOR_EVENT_READ, netlink_event_cb, NULL);
        g_nl_event_registered = 1;
        LOG_DEBUG("netlink_wait: Registered netlink event listener");
    }
    
    ctx->timer = reactor_add_timer(r, 1000, 1000, wait_timer_cb, ctx);
    if (!ctx->timer) {
        LOG_ERROR("netlink_wait: Failed to create timer");
        free(ctx);
        return -1;
    }
    
    pthread_mutex_lock(&g_list_mutex);
    ctx->next = g_ctx_list;
    g_ctx_list = ctx;
    pthread_mutex_unlock(&g_list_mutex);
    
    LOG_INFO("netlink_wait: Started waiting for interface %s (timeout=%u ms)", 
             ifname, ctx->timeout_ms);
    
    return 0;
}

void netlink_wait_cleanup(void) {
    pthread_mutex_lock(&g_list_mutex);
    
    iface_wait_ctx_t *ctx = g_ctx_list;
    g_ctx_list = NULL; // Detach immediately
    
    pthread_mutex_unlock(&g_list_mutex);
    
    // Clean detached list without holding global lock
    while (ctx) {
        iface_wait_ctx_t *next = ctx->next;
        LOG_WARN("netlink_wait: Force cleanup pending wait for %s", ctx->ifname);
        if (ctx->timer && ctx->reactor) {
            reactor_cancel_timer(ctx->reactor, ctx->timer);
        }
        free(ctx);
        ctx = next;
    }
    
    if (g_nl_event_fd >= 0) {
        if (g_nl_event_registered && g_reactor) {
            reactor_remove_fd(g_reactor, g_nl_event_fd);
        }
        close(g_nl_event_fd);
        g_nl_event_fd = -1;
    }
    g_nl_event_registered = 0;
    
    LOG_DEBUG("netlink_wait: Cleanup complete");
}

