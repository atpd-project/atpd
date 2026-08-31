#ifndef ATPD_UDS_H
#define ATPD_UDS_H

#include "reactor.h"
#include "api.h"
#include "service.h"
#include <signal.h>

#define ATPD_UDS_PATH "/data/local/tmp/atpd.sock"

typedef struct {
    atp_config_t *config;
    service_ctx_t *service;
    api_ctx_t *api;
    volatile sig_atomic_t *shutdown_requested;
} uds_dependencies_t;

int uds_init(reactor_t *r, const char *path, const uds_dependencies_t *deps);
void uds_cleanup(void);
int uds_get_fd(void);

#endif
