#ifndef ATP_SERVICE_H
#define ATP_SERVICE_H

#include "atp.h"
#include "reactor.h"

typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_RUNNING = 1,
    SERVICE_STARTING = 2,
    SERVICE_STOPPING = 3,
    SERVICE_FAILED = 4,
    SERVICE_WAIT_PROCESS = 5,
    SERVICE_WAIT_API = 6
} service_state_t;

typedef struct service_ctx service_ctx_t;

typedef void (*service_ready_cb)(service_ctx_t *ctx, void *userdata);
typedef void (*service_error_cb)(service_ctx_t *ctx, int error, const char *msg, void *userdata);

struct service_ctx {
    char bin_path[PATH_MAX];
    char conf_path[PATH_MAX];
    char log_path[PATH_MAX];
    char pid_path[PATH_MAX];
    char work_dir[PATH_MAX];
    char user[64];
    char group[64];
    int restart_cooldown_sec;
    int restart_delay_sec;
    time_t last_restart_time;
    int restart_failures;
    service_state_t state;
    int pid_fd;
    
    reactor_t *reactor;
    reactor_timer_t *wait_timer;
    reactor_timer_t *restart_timer;
    service_ready_cb on_ready;
    service_error_cb on_error;
    void *callback_userdata;
    pid_t target_pid;
    time_t start_time;
    int api_port;
    char api_addr[64];
};

int service_init(service_ctx_t *ctx, atp_config_t *cfg);

int service_start_async(service_ctx_t *ctx, reactor_t *r,
                        service_ready_cb on_ready,
                        service_error_cb on_error,
                        void *userdata);
int service_stop_async(service_ctx_t *ctx);
int service_restart_async(service_ctx_t *ctx);

int service_check(service_ctx_t *ctx);
int service_check_port(int port);
int service_get_pid(service_ctx_t *ctx);
int service_validate_config(service_ctx_t *ctx);
int service_rotate_log(service_ctx_t *ctx);
int service_acquire_pid_lock(service_ctx_t *ctx);
void service_release_pid_lock(service_ctx_t *ctx);

void service_set_cooldown(service_ctx_t *ctx, int seconds);
int service_cooldown_active(service_ctx_t *ctx);
void service_reset_failures(service_ctx_t *ctx);

pid_t service_find_process(const char *name);
int service_kill_process(pid_t pid, int signal, int wait_sec);
int service_kill_all(const char *name, int signal);

int service_monitor(service_ctx_t *ctx);
int service_get_fd(service_ctx_t *ctx);
void service_handle(service_ctx_t *ctx);

const char *service_state_string(service_state_t state);
int service_is_ready(service_ctx_t *ctx);
time_t service_get_uptime(service_ctx_t *ctx);

#endif
