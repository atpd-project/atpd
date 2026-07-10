#ifndef ATP_SERVICE_H
#define ATP_SERVICE_H

#include "atp.h"
#include "reactor.h"
#include <sys/types.h>

typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_FAILED,
    SERVICE_STOPPING
} service_state_t;

typedef struct {
    char bin_path[PATH_MAX];
    char work_dir[PATH_MAX];
    char conf_path[PATH_MAX];
    char log_path[PATH_MAX];
    char user[64];
    char group[64];
    int api_port;
    pid_t child_pid;
    int validated_pid;
    service_state_t state;
    int fail_count;
    int max_failures;
    reactor_t *reactor;
    reactor_timer_t *monitor_timer;
    reactor_timer_t *retry_timer;
    void *validate_ctx;
    struct {
        int base_delay_ms;
        int max_delay_ms;
        int current_delay_ms;
        int multiplier;
    } backoff;
} service_ctx_t;

typedef struct {
    service_ctx_t *ctx;
    int attempts;
    int max_attempts;
    reactor_timer_t *timer;
    void (*done_cb)(service_ctx_t *, void *);
    void *userdata;
} service_stop_state_t;

int service_init(service_ctx_t *ctx, atp_config_t *cfg);
int service_start_async(service_ctx_t *ctx);
int service_stop_async(service_ctx_t *ctx, void (*done_cb)(service_ctx_t *, void *), void *userdata);
int service_get_pid(service_ctx_t *ctx);
int service_is_running(service_ctx_t *ctx);
void service_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
void service_sigchld_cb(reactor_t *r, int signo, void *userdata);
const char* service_state_string(service_state_t state);

#endif
