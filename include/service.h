#ifndef ATP_SERVICE_H
#define ATP_SERVICE_H

#include "atp.h"
#include "reactor.h"
#include <sys/types.h>

typedef enum {
    SERVICE_STOPPED,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_FAILED
} service_state_t;

typedef struct service_ctx {
    char bin_path[256];
    char work_dir[256];
    char conf_path[256];
    char log_path[256];
    char user[64];
    char group[64];
    int api_port;
    
    pid_t child_pid;
    service_state_t state;
    int fail_count;
    int max_failures;
    
    reactor_t *reactor;
    reactor_timer_t *monitor_timer;
} service_ctx_t;

int service_init(service_ctx_t *ctx, atp_config_t *cfg);
int service_start_async(service_ctx_t *ctx);
int service_stop_async(service_ctx_t *ctx);
void service_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);

int service_is_running(service_ctx_t *ctx);
const char *service_state_string(service_state_t state);

#endif
