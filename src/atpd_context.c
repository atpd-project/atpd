#include "atpd_error.h"
/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * ATPd Global Context (VPN State Machine + Runtime State)
 */

#include "atpd_context.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdatomic.h>

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

const char* atpd_runtime_state_string(atpd_runtime_state_t state) {
    if (state >= ATPD_RUNTIME_STATE_UNINITIALIZED && state <= ATPD_RUNTIME_STATE_FAILED) {
        return runtime_state_names[state];
    }
    return "UNKNOWN";
}

void atpd_context_init(void) {
    atpd_error_init();
    memset(&g_atpd_ctx, 0, sizeof(g_atpd_ctx));
    
    /* VPN State - atomic init */
    atomic_init(&g_atpd_ctx.vpn_state, VPN_STATE_IDLE);
    g_atpd_ctx.xfrm_fd = -1;
    clock_gettime(CLOCK_MONOTONIC, &g_atpd_ctx.vpn_state_since);
    g_atpd_ctx.vpn_transitions = 0;
    snprintf(g_atpd_ctx.clash_desired_mode,
             sizeof(g_atpd_ctx.clash_desired_mode), "Rule");
    
    /* Runtime State */
    g_atpd_ctx.runtime_state = ATPD_RUNTIME_STATE_UNINITIALIZED;
    g_atpd_ctx.start_time = time(NULL);
    g_atpd_ctx.uptime_seconds = 0;
    g_atpd_ctx.reload_count = 0;
    g_atpd_ctx.error_count = 0;
    g_atpd_ctx.last_activity_time = time(NULL);
    
    /* Components */
    g_atpd_ctx.components.netlink_ready = false;
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
    
    LOG_INFO("ATPd Context: initialized (VPN=%s, Runtime=%s)",
             vpn_state_string(atomic_load(&g_atpd_ctx.vpn_state)),
             atpd_runtime_state_string(g_atpd_ctx.runtime_state));
}

void atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface) {
    int old_int = atomic_exchange_explicit(&g_atpd_ctx.vpn_state, (int)new_state, memory_order_acq_rel);
    vpn_state_t old_state = (vpn_state_t)old_int;

    if (old_state == new_state) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_us = (now.tv_sec - g_atpd_ctx.vpn_state_since.tv_sec) * 1000000 +
                      (now.tv_nsec - g_atpd_ctx.vpn_state_since.tv_nsec) / 1000;

    LOG_INFO("VPN_STATE: %s -> %s (IF_ID=%u, elapsed=%ldus)",
             vpn_state_string(old_state), vpn_state_string(new_state),
             if_id, elapsed_us);

    g_atpd_ctx.xfrm_if_id = if_id;
    g_atpd_ctx.vpn_state_since = now;
    g_atpd_ctx.vpn_transitions++;

    if (iface && iface[0]) {
        snprintf(g_atpd_ctx.vpn_iface, sizeof(g_atpd_ctx.vpn_iface), "%s", iface);
    }

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


uint32_t atpd_error_get_last_code(void) {
    return g_atpd_ctx.last_error.last_error_code;
}
