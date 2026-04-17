#ifndef ATP_SERVICE_H
#define ATP_SERVICE_H

#include "atp.h"

typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_RUNNING = 1,
    SERVICE_STARTING = 2,
    SERVICE_STOPPING = 3,
    SERVICE_FAILED = 4
} service_state_t;

typedef struct {
    char bin_path[PATH_MAX];
    char conf_path[PATH_MAX];
    char log_path[PATH_MAX];
    char pid_path[PATH_MAX];
    char work_dir[PATH_MAX];
    char user[64];
    char group[64];
    int restart_cooldown_sec;
    time_t last_restart_time;
    int restart_failures;
    service_state_t state;
} service_ctx_t;

int service_init(service_ctx_t *ctx, atp_config_t *cfg);
int service_start(service_ctx_t *ctx);
int service_stop(service_ctx_t *ctx);
int service_restart(service_ctx_t *ctx);
int service_check(service_ctx_t *ctx);
int service_check_port(int port);
int service_get_pid(service_ctx_t *ctx);
int service_wait_ready(service_ctx_t *ctx, int timeout_sec);
int service_validate_config(service_ctx_t *ctx);
int service_rotate_log(service_ctx_t *ctx);

void service_set_cooldown(service_ctx_t *ctx, int seconds);
int service_cooldown_active(service_ctx_t *ctx);
void service_reset_failures(service_ctx_t *ctx);

pid_t service_find_process(const char *name);
int service_kill_process(pid_t pid, int signal, int wait_sec);
int service_kill_all(const char *name, int signal);

#endif