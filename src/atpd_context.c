#include "atpd_error.h"
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * ATPd Global Context (VPN State Machine + Session List + Runtime State)
 */

#include "atpd_context.h"
#include "logger.h"
#include "session.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

atpd_context_t g_atpd_ctx;

static const char* runtime_state_names[] = {
    [ATPD_RUNTIME_STATE_UNINITIALIZED] = "UNINITIALIZED",
    [ATPD_RUNTIME_STATE_INITIALIZING]  = "INITIALIZING",
    [ATPD_RUNTIME_STATE_RUNNING]       = "RUNNING",
    [ATPD_RUNTIME_STATE_RELOADING]     = "RELOADING",
    [ATPD_RUNTIME_STATE_STOPPING]      = "STOPPING",
    [ATPD_RUNTIME_STATE_STOPPED]       = "STOPPED",
    [ATPD_RUNTIME_STATE_FAILED]        = "FAILED"
};

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

const char* atpd_runtime_state_string(atpd_runtime_state_t state) {
    if (state >= ATPD_RUNTIME_STATE_UNINITIALIZED && state <= ATPD_RUNTIME_STATE_FAILED) {
        return runtime_state_names[state];
    }
    return "UNKNOWN";
}

void atpd_context_init(void) {
    atpd_error_init();
    memset(&g_atpd_ctx, 0, sizeof(g_atpd_ctx));
    
    /* VPN State */
    g_atpd_ctx.vpn_state = VPN_STATE_IDLE;
    g_atpd_ctx.xfrm_fd = -1;
    clock_gettime(CLOCK_MONOTONIC, &g_atpd_ctx.vpn_state_since);
    g_atpd_ctx.vpn_teardown_cb = atpd_vpn_killswitch;
    g_atpd_ctx.vpn_transitions = 0;
    g_atpd_ctx.splice_bytes_total = 0;
    
    /* eBPF State */
    g_atpd_ctx.ebpf_state = EBPF_STATE_UNINITIALIZED;
    g_atpd_ctx.ebpf_enabled = false;
    g_atpd_ctx.ebpf_probed = false;
    g_atpd_ctx.ebpf_pin_dir[0] = '\0';
    
    /* Runtime State */
    g_atpd_ctx.runtime_state = ATPD_RUNTIME_STATE_UNINITIALIZED;
    g_atpd_ctx.start_time = time(NULL);
    g_atpd_ctx.uptime_seconds = 0;
    g_atpd_ctx.reload_count = 0;
    g_atpd_ctx.error_count = 0;
    g_atpd_ctx.last_activity_time = time(NULL);
    
    /* Components */
    g_atpd_ctx.components.netlink_ready = false;
    g_atpd_ctx.components.ebpf_ready = false;
    g_atpd_ctx.components.service_ready = false;
    g_atpd_ctx.components.api_ready = false;
    g_atpd_ctx.components.reactor_ready = false;
    
    /* Stats */
    g_atpd_ctx.stats.events_processed = 0;
    g_atpd_ctx.stats.timers_fired = 0;
    g_atpd_ctx.stats.signals_received = 0;
    g_atpd_ctx.stats.errors_total = 0;
    g_atpd_ctx.stats.bytes_rx = 0;
    g_atpd_ctx.stats.bytes_tx = 0;
    
    /* Error */
    g_atpd_ctx.last_error.last_error_code = 0;
    g_atpd_ctx.last_error.last_error_msg[0] = '\0';
    g_atpd_ctx.last_error.last_error_time = 0;
    
    LOG_INFO("ATPd Context: initialized (VPN=%s, eBPF=%s, Runtime=%s)",
             vpn_state_string(g_atpd_ctx.vpn_state),
             ebpf_state_string(g_atpd_ctx.ebpf_state),
             atpd_runtime_state_string(g_atpd_ctx.runtime_state));
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

void atpd_ebpf_state_transition(ebpf_state_t new_state) {
    ebpf_state_t old_state = g_atpd_ctx.ebpf_state;

    if (old_state == new_state) return;

    LOG_INFO("EBPF_STATE: %s -> %s",
             ebpf_state_string(old_state), ebpf_state_string(new_state));

    g_atpd_ctx.ebpf_state = new_state;
}

/* === Runtime State Functions === */

void atpd_runtime_state_transition(atpd_runtime_state_t new_state) {
    atpd_runtime_state_t old = g_atpd_ctx.runtime_state;
    if (old == new_state) return;
    
    g_atpd_ctx.runtime_state = new_state;
    g_atpd_ctx.last_activity_time = time(NULL);
    
    if (new_state == ATPD_RUNTIME_STATE_RUNNING) {
        g_atpd_ctx.start_time = time(NULL);
    }
    
    LOG_INFO("RUNTIME_STATE: %s -> %s",
             atpd_runtime_state_string(old),
             atpd_runtime_state_string(new_state));
}

int atpd_runtime_is_running(void) {
    return g_atpd_ctx.runtime_state == ATPD_RUNTIME_STATE_RUNNING;
}

int atpd_runtime_can_reload(void) {
    return g_atpd_ctx.runtime_state == ATPD_RUNTIME_STATE_RUNNING ||
           g_atpd_ctx.runtime_state == ATPD_RUNTIME_STATE_RELOADING;
}

void atpd_runtime_update_uptime(void) {
    if (g_atpd_ctx.start_time > 0) {
        g_atpd_ctx.uptime_seconds = time(NULL) - g_atpd_ctx.start_time;
    }
}

uint64_t atpd_runtime_get_uptime(void) {
    atpd_runtime_update_uptime();
    return g_atpd_ctx.uptime_seconds;
}

/* === Component Status Functions === */

void atpd_component_set_ready(const char *name, int ready) {
    if (strcmp(name, "netlink") == 0) {
        g_atpd_ctx.components.netlink_ready = ready;
    } else if (strcmp(name, "ebpf") == 0) {
        g_atpd_ctx.components.ebpf_ready = ready;
    } else if (strcmp(name, "service") == 0) {
        g_atpd_ctx.components.service_ready = ready;
    } else if (strcmp(name, "api") == 0) {
        g_atpd_ctx.components.api_ready = ready;
    } else if (strcmp(name, "reactor") == 0) {
        g_atpd_ctx.components.reactor_ready = ready;
    }
    
    LOG_DEBUG("Component '%s' %s", name, ready ? "ready" : "not ready");
}

int atpd_component_is_ready(const char *name) {
    if (strcmp(name, "netlink") == 0) return g_atpd_ctx.components.netlink_ready;
    if (strcmp(name, "ebpf") == 0) return g_atpd_ctx.components.ebpf_ready;
    if (strcmp(name, "service") == 0) return g_atpd_ctx.components.service_ready;
    if (strcmp(name, "api") == 0) return g_atpd_ctx.components.api_ready;
    if (strcmp(name, "reactor") == 0) return g_atpd_ctx.components.reactor_ready;
    return 0;
}

/* === Statistics Functions === */

void atpd_stats_increment_events(void) {
    g_atpd_ctx.stats.events_processed++;
}

void atpd_stats_increment_timers(void) {
    g_atpd_ctx.stats.timers_fired++;
}

void atpd_stats_increment_signals(void) {
    g_atpd_ctx.stats.signals_received++;
}

void atpd_stats_increment_errors(void) {
    g_atpd_ctx.stats.errors_total++;
}

void atpd_stats_add_bytes(uint64_t rx, uint64_t tx) {
    g_atpd_ctx.stats.bytes_rx += rx;
    g_atpd_ctx.stats.bytes_tx += tx;
}

/* === Error Functions === */

void atpd_error_record(int code, const char *msg) {
    g_atpd_ctx.last_error.last_error_code = code;
    strncpy(g_atpd_ctx.last_error.last_error_msg, msg,
            sizeof(g_atpd_ctx.last_error.last_error_msg) - 1);
    g_atpd_ctx.last_error.last_error_msg[sizeof(g_atpd_ctx.last_error.last_error_msg) - 1] = '\0';
    g_atpd_ctx.last_error.last_error_time = time(NULL);
    g_atpd_ctx.error_count++;
    
    LOG_ERROR("Error recorded: code=%d, msg=%s", code, msg);
}

const char* atpd_error_get_last(void) {
    return g_atpd_ctx.last_error.last_error_msg;
}

uint32_t atpd_error_get_last_code(void) {
    return g_atpd_ctx.last_error.last_error_code;
}

/* === Session Functions === */

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
