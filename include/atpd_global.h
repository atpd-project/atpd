#ifndef ATPD_GLOBAL_H
#define ATPD_GLOBAL_H

#include "atp.h"
#include "api.h"
#include "reactor.h"
#include "service.h"

typedef struct {
    atp_config_t config;
    api_ctx_t api_ctx;
    reactor_t *reactor;
    service_ctx_t *svc;
    volatile sig_atomic_t running;
    volatile sig_atomic_t reload;
    volatile sig_atomic_t show_status;
} atpd_global_t;

extern atpd_global_t g_atpd;

#define g_config g_atpd.config
#define g_api_ctx g_atpd.api_ctx
#define g_reactor g_atpd.reactor
#define g_svc g_atpd.svc
#define g_running g_atpd.running
#define g_reload g_atpd.reload
#define g_show_status g_atpd.show_status

#endif
