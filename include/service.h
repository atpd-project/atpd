#ifndef ATP_SERVICE_H
#define ATP_SERVICE_H

#include "atp.h"
#include "reactor.h"
#include <sys/types.h>
#include <signal.h>

// Forward declaration for async validation
typedef struct async_validate_ctx async_validate_ctx_t;

typedef enum {
    SERVICE_STOPPED,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_FAILED
} service_state_t;

typedef struct {
    int base_delay_ms;
    int max_delay_ms;
    int current_delay_ms;
    int multiplier;
} backoff_t;

typedef struct service_ctx {
    char bin_path[256];
    char work_dir[256];
    char conf_path[256];
    char log_path[256];
    char user[64];
    char group[64];
    int api_port;
    
    pid_t child_pid;
    pid_t validated_pid;
    service_state_t state;
    int fail_count;
    int max_failures;
    
    backoff_t backoff;
    
    reactor_t *reactor;
    reactor_timer_t *monitor_timer;
    
    async_validate_ctx_t *validate_ctx;  // Phase 3: async validation
} service_ctx_t;

int service_init(service_ctx_t *ctx, atp_config_t *cfg);
int service_start_async(service_ctx_t *ctx);
int service_stop_async(service_ctx_t *ctx);
void service_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
void service_sigchld_cb(reactor_t *r, int signo, void *userdata);

int service_get_pid(service_ctx_t *ctx);
int service_is_running(service_ctx_t *ctx);
const char *service_state_string(service_state_t state);

#endif
