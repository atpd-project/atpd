/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Shutdown handling
 */

#include "atpd_global.h"
#include "logger.h"
#include "boxbpf.h"
#include "tproxy.h"
#include "service.h"
#include "reactor.h"
#include "cleanup.h"
#include "atpd_context.h"

void atpd_shutdown_cleanup(void) {
    if (g_config.ebpf.ready) {
        LOG_INFO("Cleaning up eBPF CNIP...");
        boxbpf_clear();
        g_config.ebpf.ready = 0;
        g_atpd_ctx.ebpf_enabled = false;
        atpd_ebpf_state_transition(EBPF_STATE_UNINITIALIZED);
    }
    
    tproxy_cleanup_all(&g_config);
    atp_cleanup_manual(&g_config);
    
    LOG_INFO("Shutdown cleanup complete");
}

void atpd_shutdown_service(service_ctx_t **svc) {
    if (!svc || !*svc) return;
    
    service_stop_async(*svc, NULL, NULL);
    free(*svc);
    *svc = NULL;
}
