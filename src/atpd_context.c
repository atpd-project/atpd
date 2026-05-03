#include "atpd_context.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

void atpd_context_init(void) {
    memset(&g_atpd_ctx, 0, sizeof(g_atpd_ctx));
    g_atpd_ctx.vpn_state = VPN_STATE_IDLE;
    g_atpd_ctx.xfrm_fd = -1;
    clock_gettime(CLOCK_MONOTONIC, &g_atpd_ctx.vpn_state_since);
    LOG_INFO("ATPd Context: initialized (VPN state = IDLE)");
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
    
    /* Kill-switch: TEARDOWN triggers cleanup */
    if (new_state == VPN_STATE_TEARDOWN && g_atpd_ctx.vpn_teardown_cb) {
        LOG_WARN("VPN_STATE: Kill-switch activated, cleaning up sessions");
        g_atpd_ctx.vpn_teardown_cb();
    }
}
