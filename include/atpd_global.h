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

#endif
