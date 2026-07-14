#include "cleanup.h"
#include "logger.h"
#include "tproxy.h"
#include "boxbpf.h"
#include "atp.h"
#include <stdlib.h>
#include <unistd.h>

static atp_config_t *g_cleanup_cfg = NULL;
static int g_cleanup_registered = 0;

static void atp_cleanup_handler(void) {
    if (!g_cleanup_cfg) return;

    LOG_INFO("Cleanup: removing iptables rules and eBPF pins");

    if (g_cleanup_cfg->ebpf.ready) {
        boxbpf_clear();
        LOG_INFO("Cleanup: eBPF pins removed");
    }

    tproxy_cleanup_all(g_cleanup_cfg);
    LOG_INFO("Cleanup: iptables rules removed");

    tproxy_cleanup_xfrm_bypass(g_cleanup_cfg);
    LOG_INFO("Cleanup: XFRM bypass removed");
}

void atp_register_cleanup(atp_config_t *cfg) {
    if (!cfg || g_cleanup_registered) return;

    g_cleanup_cfg = cfg;
    g_cleanup_registered = 1;
    atexit(atp_cleanup_handler);
    LOG_DEBUG("Cleanup: atexit handler registered");
}

void atp_cleanup_all(void) {
    atp_cleanup_handler();
}

void atp_cleanup_manual(atp_config_t *cfg) {
    if (!cfg) return;

    LOG_INFO("Manual cleanup: removing iptables rules and eBPF pins");

    boxbpf_clear();
    tproxy_cleanup_all(cfg);
    tproxy_cleanup_xfrm_bypass(cfg);

    LOG_INFO("Manual cleanup completed");
}
