#ifndef ATPD_INIT_H
#define ATPD_INIT_H

#include "atp.h"
#include "atpd_context.h"
#include "reactor.h"
#include "service.h"
#include "api.h"
#include "cli.h"

typedef struct atpd_init_context {
    atp_config_t *config;
    atpd_context_t *ctx;
    reactor_t *reactor;
    service_ctx_t *service;
    api_ctx_t *api;
    atp_options_t *opts;
} atpd_init_context_t;

typedef enum {
    INIT_PHASE_CONFIG = 0,
    INIT_PHASE_LOGGER,
    INIT_PHASE_NETLINK,
    INIT_PHASE_FILTER,
    INIT_PHASE_SERVICE,
    INIT_PHASE_API,
    INIT_PHASE_READY,
    INIT_PHASE_MAX
} init_phase_t;

typedef int (*init_phase_handler_t)(atpd_init_context_t *ctx);

typedef struct {
    init_phase_t phase;
    const char *name;
    init_phase_handler_t handler;
    int required;
    int skip_on_failure;
} init_phase_config_t;

int atpd_init_phase_config(atpd_init_context_t *ctx);
int atpd_init_phase_logger(atpd_init_context_t *ctx);
int atpd_init_phase_netlink(atpd_init_context_t *ctx);
int atpd_init_phase_filter(atpd_init_context_t *ctx);
int atpd_init_phase_service(atpd_init_context_t *ctx);
int atpd_init_phase_api(atpd_init_context_t *ctx);
int atpd_init_phase_ready(atpd_init_context_t *ctx);

int atpd_init_run(atpd_init_context_t *ctx);
int atpd_init_rollback(atpd_init_context_t *ctx, init_phase_t phase);

#endif
