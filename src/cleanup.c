#include "cleanup.h"
#include "logger.h"
#include "atp.h"
#include <stdlib.h>
#include <unistd.h>

static atp_config_t *g_cleanup_cfg = NULL;
static int g_cleanup_registered = 0;

static void atp_cleanup_handler(void) {
    if (!g_cleanup_cfg) return;
    LOG_DEBUG("Cleanup: exit handler invoked");
}

void atp_register_cleanup(atp_config_t *cfg) {
    if (!cfg || g_cleanup_registered) return;

    g_cleanup_cfg = cfg;
    g_cleanup_registered = 1;
    atexit(atp_cleanup_handler);
}

void atp_cleanup_all(void) {
    atp_cleanup_handler();
}

void atp_cleanup_manual(atp_config_t *cfg) {
    (void)cfg;
    LOG_INFO("Manual cleanup: no ATPD dataplane resources to clean");
}
