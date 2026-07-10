/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * ATPd Global Context (VPN State Machine + Session List)
 */

#include "atpd_context.h"
#include "logger.h"
#include "session.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

atpd_context_t g_atpd_ctx;

const char* vpn_state_string(vpn_state_t state) {
    switch (state) {
        case VPN_STATE_IDLE:       return "IDLE";
        case VPN_STATE_PREDICTING: return "PREDICTING";
        case VPN_STATE_READY:      return "READY";
        case VPN_STATE_TEARDOWN:   return "TEARDOWN";
        default:                   return "UNKNOWN";
    }
}

const char* ebpf_state_string(ebpf_state_t state) {
    switch (state) {
        case EBPF_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case EBPF_STATE_LOADING:       return "LOADING";
        case EBPF_STATE_READY:         return "READY";
        case EBPF_STATE_FAILED:        return "FAILED";
        case EBPF_STATE_DISABLED:      return "DISABLED";
        default:                       return "UNKNOWN";
    }
}

void atpd_context_init(void) {
    memset(&g_atpd_ctx, 0, sizeof(g_atpd_ctx));
    g_atpd_ctx.vpn_state = VPN_STATE_IDLE;
    g_atpd_ctx.xfrm_fd = -1;
    clock_gettime(CLOCK_MONOTONIC, &g_atpd_ctx.vpn_state_since);
    g_atpd_ctx.vpn_teardown_cb = atpd_vpn_killswitch;
    g_atpd_ctx.ebpf_state = EBPF_STATE_UNINITIALIZED;
    g_atpd_ctx.ebpf_enabled = false;
    g_atpd_ctx.ebpf_probed = false;
    LOG_INFO("ATPd Context: initialized (VPN state = IDLE, eBPF state = UNINITIALIZED)");
}

void atpd_ebpf_state_transition(ebpf_state_t new_state) {
    ebpf_state_t old_state = g_atpd_ctx.ebpf_state;

    if (old_state == new_state) return;

    LOG_INFO("EBPF_STATE: %s -> %s",
             ebpf_state_string(old_state), ebpf_state_string(new_state));

    g_atpd_ctx.ebpf_state = new_state;
}

void atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface) {
    vpn_state_t old_state = g_atpd_ctx.vpn_state;

    if (old_state == new_state) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_us = (now.tv_sec - g_atpd_ctx.vpn_state_since.tv_sec) * 1000000 +
                      (now.tv_nsec - g_atpd_ctx.vpn_state_since.tv_nsec) / 1000;

    LOG_INFO("VPN_STATE: %s -> %s (IF_ID=%u, elapsed=%ldus)",
             vpn_state_string(old_state), vpn_state_string(new_state),
             if_id, elapsed_us);

    g_atpd_ctx.vpn_state = new_state;
    g_atpd_ctx.xfrm_if_id = if_id;
    g_atpd_ctx.vpn_state_since = now;
    g_atpd_ctx.vpn_transitions++;

    if (iface && iface[0]) {
        snprintf(g_atpd_ctx.vpn_iface, sizeof(g_atpd_ctx.vpn_iface), "%s", iface);
    }

    if (new_state == VPN_STATE_TEARDOWN && g_atpd_ctx.vpn_teardown_cb) {
        LOG_WARN("VPN_STATE: Kill-switch activated, cleaning up sessions");
        g_atpd_ctx.vpn_teardown_cb();
    }
}

void atpd_session_register_to_ctx(struct atpd_session *s) {
    if (!s) return;

    struct atpd_session_list *node = calloc(1, sizeof(struct atpd_session_list));
    if (!node) {
        LOG_ERROR("ATPd Context: failed to allocate session list node");
        return;
    }
    node->session = s;
    node->next = g_atpd_ctx.sessions;
    g_atpd_ctx.sessions = node;

    LOG_DEBUG("ATPd Context: session registered");
}

void atpd_session_unregister_from_ctx(struct atpd_session *s) {
    if (!s || !g_atpd_ctx.sessions) return;

    struct atpd_session_list **pp = &g_atpd_ctx.sessions;
    while (*pp) {
        if ((*pp)->session == s) {
            struct atpd_session_list *to_free = *pp;
            *pp = (*pp)->next;
            free(to_free);
            LOG_DEBUG("ATPd Context: session unregistered");
            return;
        }
        pp = &(*pp)->next;
    }
}

void atpd_vpn_killswitch(void) {
    int closed = 0;

    struct atpd_session *session_ptrs[256];
    int count = 0;
    struct atpd_session_list *node = g_atpd_ctx.sessions;

    while (node && count < 256) {
        if (node->session) {
            session_ptrs[count++] = node->session;
        }
        node = node->next;
    }

    for (int i = 0; i < count; i++) {
        atpd_session_destroy(session_ptrs[i]);
        closed++;
    }

    node = g_atpd_ctx.sessions;
    while (node) {
        struct atpd_session_list *next = node->next;
        if (node->session) {
            atpd_session_destroy(node->session);
            closed++;
        }
        free(node);
        node = next;
    }
    g_atpd_ctx.sessions = NULL;

    LOG_WARN("ATPd Context: Kill-switch closed %d sessions", closed);
}
