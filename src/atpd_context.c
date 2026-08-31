/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Daemon lifecycle and VPN observation state.
 */

#include "atpd_context.h"
#include "logger.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct atpd_context {
    pthread_mutex_t lock;
    bool initialized;

    atpd_vpn_snapshot_t vpn;
    atpd_vpn_mode_callback_t vpn_mode_callback;
    void *vpn_mode_userdata;
    void (*vpn_teardown_callback)(void);

    atpd_runtime_state_t runtime_state;
    struct timespec started_at_mono;
};

static struct atpd_context g_context = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .vpn = {
        .state = VPN_STATE_IDLE
    },
    .runtime_state = ATPD_RUNTIME_STATE_UNINITIALIZED
};

static const char *runtime_state_names[] = {
    [ATPD_RUNTIME_STATE_UNINITIALIZED] = "UNINITIALIZED",
    [ATPD_RUNTIME_STATE_INITIALIZING] = "INITIALIZING",
    [ATPD_RUNTIME_STATE_RUNNING] = "RUNNING",
    [ATPD_RUNTIME_STATE_RELOADING] = "RELOADING",
    [ATPD_RUNTIME_STATE_STOPPING] = "STOPPING",
    [ATPD_RUNTIME_STATE_STOPPED] = "STOPPED",
    [ATPD_RUNTIME_STATE_FAILED] = "FAILED"
};

const char *vpn_state_string(vpn_state_t state) {
    switch (state) {
        case VPN_STATE_IDLE:       return "IDLE";
        case VPN_STATE_PREDICTING: return "PREDICTING";
        case VPN_STATE_READY:      return "READY";
        case VPN_STATE_TEARDOWN:   return "TEARDOWN";
        default:                   return "UNKNOWN";
    }
}

const char *atpd_runtime_state_string(atpd_runtime_state_t state) {
    if (state >= ATPD_RUNTIME_STATE_UNINITIALIZED &&
        state <= ATPD_RUNTIME_STATE_FAILED) {
        return runtime_state_names[state];
    }
    return "UNKNOWN";
}

int atpd_context_init(void) {
    struct timespec now;

    pthread_mutex_lock(&g_context.lock);
    if (g_context.initialized) {
        pthread_mutex_unlock(&g_context.lock);
        LOG_ERROR("ATPD context initialization requested more than once");
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    g_context.initialized = true;
    g_context.vpn.state = VPN_STATE_IDLE;
    g_context.vpn.if_id = 0;
    g_context.vpn.iface[0] = '\0';
    g_context.vpn.changed_at = now;
    g_context.vpn.transitions = 0;
    g_context.vpn_mode_callback = NULL;
    g_context.vpn_mode_userdata = NULL;
    g_context.vpn_teardown_callback = NULL;
    g_context.runtime_state = ATPD_RUNTIME_STATE_UNINITIALIZED;
    g_context.started_at_mono = now;
    pthread_mutex_unlock(&g_context.lock);

    LOG_INFO("ATPD context initialized (VPN=%s, Runtime=%s)",
             vpn_state_string(VPN_STATE_IDLE),
             atpd_runtime_state_string(ATPD_RUNTIME_STATE_UNINITIALIZED));
    return 0;
}

static bool runtime_transition_allowed(atpd_runtime_state_t old_state,
                                       atpd_runtime_state_t new_state) {
    switch (old_state) {
        case ATPD_RUNTIME_STATE_UNINITIALIZED:
            return new_state == ATPD_RUNTIME_STATE_INITIALIZING;
        case ATPD_RUNTIME_STATE_INITIALIZING:
            return new_state == ATPD_RUNTIME_STATE_RUNNING ||
                   new_state == ATPD_RUNTIME_STATE_FAILED ||
                   new_state == ATPD_RUNTIME_STATE_STOPPING;
        case ATPD_RUNTIME_STATE_RUNNING:
            return new_state == ATPD_RUNTIME_STATE_RELOADING ||
                   new_state == ATPD_RUNTIME_STATE_STOPPING ||
                   new_state == ATPD_RUNTIME_STATE_FAILED;
        case ATPD_RUNTIME_STATE_RELOADING:
            return new_state == ATPD_RUNTIME_STATE_RUNNING ||
                   new_state == ATPD_RUNTIME_STATE_STOPPING;
        case ATPD_RUNTIME_STATE_STOPPING:
            return new_state == ATPD_RUNTIME_STATE_STOPPED;
        case ATPD_RUNTIME_STATE_FAILED:
            return new_state == ATPD_RUNTIME_STATE_STOPPING ||
                   new_state == ATPD_RUNTIME_STATE_STOPPED;
        case ATPD_RUNTIME_STATE_STOPPED:
            return false;
    }
    return false;
}

int atpd_runtime_state_transition(atpd_runtime_state_t new_state) {
    atpd_runtime_state_t old_state;

    if (new_state < ATPD_RUNTIME_STATE_UNINITIALIZED ||
        new_state > ATPD_RUNTIME_STATE_FAILED) {
        LOG_ERROR("Invalid runtime state %d", new_state);
        return -1;
    }

    pthread_mutex_lock(&g_context.lock);
    old_state = g_context.runtime_state;
    if (old_state == new_state) {
        pthread_mutex_unlock(&g_context.lock);
        return 0;
    }
    if (!runtime_transition_allowed(old_state, new_state)) {
        pthread_mutex_unlock(&g_context.lock);
        LOG_ERROR("Rejected runtime transition %s -> %s",
                  atpd_runtime_state_string(old_state),
                  atpd_runtime_state_string(new_state));
        return -1;
    }
    g_context.runtime_state = new_state;
    pthread_mutex_unlock(&g_context.lock);

    LOG_INFO("RUNTIME_STATE: %s -> %s",
             atpd_runtime_state_string(old_state),
             atpd_runtime_state_string(new_state));
    return 0;
}

int atpd_runtime_is_running(void) {
    int running;

    pthread_mutex_lock(&g_context.lock);
    running = g_context.runtime_state == ATPD_RUNTIME_STATE_RUNNING;
    pthread_mutex_unlock(&g_context.lock);
    return running;
}

int atpd_runtime_can_reload(void) {
    int can_reload;

    pthread_mutex_lock(&g_context.lock);
    can_reload = g_context.runtime_state == ATPD_RUNTIME_STATE_RUNNING;
    pthread_mutex_unlock(&g_context.lock);
    return can_reload;
}

uint64_t atpd_runtime_get_uptime(void) {
    struct timespec started;
    struct timespec now;

    pthread_mutex_lock(&g_context.lock);
    started = g_context.started_at_mono;
    pthread_mutex_unlock(&g_context.lock);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec < started.tv_sec ||
        (now.tv_sec == started.tv_sec && now.tv_nsec < started.tv_nsec)) {
        return 0;
    }
    return (uint64_t)(now.tv_sec - started.tv_sec);
}

int atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface) {
    atpd_vpn_mode_callback_t mode_callback;
    void (*teardown_callback)(void);
    void *userdata;
    vpn_state_t old_state;
    char callback_iface[sizeof(g_context.vpn.iface)];
    struct timespec now;
    bool identity_changed;
    bool teardown_edge;

    if (new_state < VPN_STATE_IDLE || new_state > VPN_STATE_TEARDOWN) {
        LOG_ERROR("Invalid VPN state %d", new_state);
        return -1;
    }

    callback_iface[0] = '\0';
    if (iface && iface[0]) {
        snprintf(callback_iface, sizeof(callback_iface), "%s", iface);
    }

    pthread_mutex_lock(&g_context.lock);
    old_state = g_context.vpn.state;
    identity_changed = g_context.vpn.if_id != if_id ||
                       strcmp(g_context.vpn.iface, callback_iface) != 0;
    if (old_state == new_state && !identity_changed) {
        pthread_mutex_unlock(&g_context.lock);
        return 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    g_context.vpn.state = new_state;
    g_context.vpn.if_id = if_id;
    snprintf(g_context.vpn.iface, sizeof(g_context.vpn.iface), "%s", callback_iface);
    g_context.vpn.changed_at = now;
    g_context.vpn.transitions++;
    mode_callback = g_context.vpn_mode_callback;
    teardown_callback = g_context.vpn_teardown_callback;
    userdata = g_context.vpn_mode_userdata;
    teardown_edge = old_state != VPN_STATE_TEARDOWN &&
                    new_state == VPN_STATE_TEARDOWN;
    pthread_mutex_unlock(&g_context.lock);

    LOG_INFO("VPN_STATE: %s -> %s (IF_ID=%u)",
             vpn_state_string(old_state), vpn_state_string(new_state), if_id);
    if (mode_callback) {
        mode_callback(new_state, callback_iface[0] ? callback_iface : NULL, userdata);
    }
    if (teardown_edge && teardown_callback) {
        LOG_WARN("VPN_STATE: teardown observer requested session cleanup");
        teardown_callback();
    }
    return 0;
}

void atpd_vpn_get_snapshot(atpd_vpn_snapshot_t *out) {
    if (!out) return;

    pthread_mutex_lock(&g_context.lock);
    *out = g_context.vpn;
    pthread_mutex_unlock(&g_context.lock);
}

void atpd_set_vpn_mode_callback(atpd_vpn_mode_callback_t callback, void *userdata) {
    pthread_mutex_lock(&g_context.lock);
    g_context.vpn_mode_callback = callback;
    g_context.vpn_mode_userdata = userdata;
    pthread_mutex_unlock(&g_context.lock);
}

void atpd_set_vpn_teardown_callback(void (*callback)(void)) {
    pthread_mutex_lock(&g_context.lock);
    g_context.vpn_teardown_callback = callback;
    pthread_mutex_unlock(&g_context.lock);
}
