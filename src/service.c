/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Service Manager - Industrial-grade async state machine
 * Features: PID validation, SIGCHLD integration, exponential backoff,
 *           circuit breaker, health check, configurable timeouts
 */

#include "service.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include "async_validate.h"

/* Forward declarations */
void service_schedule_retry(service_ctx_t *ctx);

#define MAX_LOG_SIZE (10 * 1024 * 1024)

static void backoff_init(backoff_t *b) {
    b->base_delay_ms = 1000;
    b->max_delay_ms = 60000;
    b->current_delay_ms = 1000;
    b->multiplier = 2;
}

static void backoff_reset(backoff_t *b) {
    b->current_delay_ms = b->base_delay_ms;
}

static int backoff_next(backoff_t *b) {
    int delay = b->current_delay_ms;
    b->current_delay_ms *= b->multiplier;
    if (b->current_delay_ms > b->max_delay_ms) {
        b->current_delay_ms = b->max_delay_ms;
    }
    return delay;
}

static void circuit_breaker_reset(circuit_breaker_t *cb) {
    cb->consecutive_failures = 0;
    cb->circuit_open = 0;
    cb->last_failure_time = 0;
}

static int circuit_breaker_should_allow(circuit_breaker_t *cb) {
    if (!cb->circuit_open) return 1;
    if (time(NULL) - cb->last_failure_time > cb->cooldown_seconds) {
        cb->circuit_open = 0;
        cb->consecutive_failures = 0;
        LOG_INFO("Circuit breaker: closed (cooldown expired)");
        return 1;
    }
    return 0;
}

static void circuit_breaker_record_failure(circuit_breaker_t *cb) {
    cb->consecutive_failures++;
    cb->last_failure_time = time(NULL);
    if (cb->consecutive_failures >= cb->threshold && !cb->circuit_open) {
        cb->circuit_open = 1;
        LOG_WARN("Circuit breaker: opened after %d failures", cb->consecutive_failures);
    }
}

static void circuit_breaker_record_success(circuit_breaker_t *cb) {
    if (cb->consecutive_failures > 0) {
        cb->consecutive_failures = 0;
    }
    if (cb->circuit_open) {
        cb->circuit_open = 0;
        LOG_INFO("Circuit breaker: closed (success)");
    }
}

static int wait_connect_result(int sock, int timeout_ms) {
    struct pollfd pfd = {
        .fd = sock,
        .events = POLLOUT
    };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return 0;

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return 0;
    }

    if (!(pfd.revents & POLLOUT)) {
        return 0;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return 0;
    }

    return err == 0;
}

static int validate_process(service_ctx_t *ctx, pid_t pid) {
    char path[256];
    char exe_buf[256];
    char cwd_buf[PATH_MAX];
    ssize_t len;

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    len = readlink(path, exe_buf, sizeof(exe_buf) - 1);
    if (len < 0) {
        LOG_DEBUG("Service: readlink failed for PID %d: %s", pid, strerror(errno));
        return 0;
    }
    exe_buf[len] = '\0';

    if (strcmp(exe_buf, ctx->bin_path) != 0) {
        LOG_WARN("Service: PID %d exe mismatch: expected %s, got %s",
                 pid, ctx->bin_path, exe_buf);
        return 0;
    }

    snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
    len = readlink(path, cwd_buf, sizeof(cwd_buf) - 1);
    if (len < 0) return 0;
    cwd_buf[len] = '\0';

    if (strcmp(cwd_buf, ctx->work_dir) != 0) {
        LOG_WARN("Service: PID %d cwd mismatch: expected %s, got %s",
                 pid, ctx->work_dir, cwd_buf);
        return 0;
    }

    LOG_INFO("Service: PID %d validated successfully", pid);
    return 1;
}

static int service_is_alive(service_ctx_t *ctx) {
    if (!ctx || ctx->child_pid <= 0) return 0;
    return kill(ctx->child_pid, 0) == 0;
}

static int service_probe_port(int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) return 0;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (ret == 0) {
        close(sock);
        return 1;
    }

    if (errno == EINPROGRESS) {
        int ok = wait_connect_result(sock, 3000);
        close(sock);
        return ok;
    }

    close(sock);
    return 0;
}

static int service_api_health_check(service_ctx_t *ctx) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) return 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        close(sock);
        return 0;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        close(sock);
        return 0;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ctx->api_port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) }
    };

    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (ret == 0) {
        close(sock);
        return 1;
    }

    if (errno == EINPROGRESS) {
        int ok = wait_connect_result(sock, 2000);
        close(sock);
        return ok;
    }

    close(sock);
    return 0;
}

static int service_binary_exists(service_ctx_t *ctx) {
    return access(ctx->bin_path, X_OK) == 0;
}

void service_rotate_log(service_ctx_t *ctx) {
    struct stat st;
    char backup_path[512];
    char timestamp[64];
    time_t now;
    struct tm *tm_info;

    if (stat(ctx->log_path, &st) != 0) return;
    if (st.st_size < MAX_LOG_SIZE) return;

    now = time(NULL);
    tm_info = localtime(&now);
    if (!tm_info) {
        LOG_WARN("Service: failed to get localtime for log rotation");
        return;
    }

    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm_info);
    snprintf(backup_path, sizeof(backup_path), "%s.%s", ctx->log_path, timestamp);

    if (rename(ctx->log_path, backup_path) == 0) {
        LOG_INFO("Service: rotated log to %s (size: %ld bytes)",
                 backup_path, (long)st.st_size);
    } else {
        LOG_WARN("Service: failed to rotate log: %s", strerror(errno));
    }
}

static void free_service_args(char **args) {
    if (!args) return;
    for (int i = 0; args[i]; i++) {
        free(args[i]);
    }
    free(args);
}

static char** build_service_args(service_ctx_t *ctx) {
    char **args = calloc(30, sizeof(char *));
    if (!args) return NULL;

    int idx = 0;

    char *arg = strdup(ctx->bin_path);
    if (!arg) { free_service_args(args); return NULL; }
    args[idx++] = arg;

    arg = strdup("run");
    if (!arg) { free_service_args(args); return NULL; }
    args[idx++] = arg;

    /* Respect working directory */
    if (ctx->work_dir[0]) {
        arg = strdup("-D");
        if (!arg) { free_service_args(args); return NULL; }
        args[idx++] = arg;

        arg = strdup(ctx->work_dir);
        if (!arg) { free_service_args(args); return NULL; }
        args[idx++] = arg;
    }

    /* Check if user already provided -c / -C in service_args */
    int has_custom_c = (ctx->service_args[0] && (strstr(ctx->service_args, "-c") || strstr(ctx->service_args, "-C")));
    if (!has_custom_c && ctx->conf_path[0]) {
        arg = strdup("-c");
        if (!arg) { free_service_args(args); return NULL; }
        args[idx++] = arg;

        arg = strdup(ctx->conf_path);
        if (!arg) { free_service_args(args); return NULL; }
        args[idx++] = arg;
    }

    if (ctx->service_args[0]) {
        char args_copy[512];
        snprintf(args_copy, sizeof(args_copy), "%s", ctx->service_args);

        char *saveptr = NULL;
        char *token = strtok_r(args_copy, " ", &saveptr);

        while (token && idx < 28) {
            arg = strdup(token);
            if (!arg) { free_service_args(args); return NULL; }
            args[idx++] = arg;
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    args[idx] = NULL;
    return args;
}

static void set_service_environment(service_ctx_t *ctx) {
    if (!ctx->service_env[0]) return;

    char env_copy[512];
    snprintf(env_copy, sizeof(env_copy), "%s", ctx->service_env);

    char *saveptr = NULL;
    char *token = strtok_r(env_copy, " ", &saveptr);

    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            setenv(token, eq + 1, 1);
        }
        token = strtok_r(NULL, " ", &saveptr);
    }
}

static int service_spawn(service_ctx_t *ctx) {
    if (!service_binary_exists(ctx)) {
        LOG_ERROR("Service: binary not found or not executable: %s", ctx->bin_path);
        return -1;
    }

    service_rotate_log(ctx);

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("Service: fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGPIPE, &sa, NULL) != 0) {
            LOG_ERROR("Service: sigaction(SIGPIPE) failed: %s", strerror(errno));
            _exit(127);
        }

        umask(027);

        if (setsid() < 0) {
            LOG_ERROR("Service: setsid failed: %s", strerror(errno));
            _exit(127);
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        int log_fd = open(ctx->log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (log_fd >= 0) {
            if (dup2(log_fd, STDOUT_FILENO) < 0) {
                close(log_fd);
                _exit(127);
            }
            if (dup2(log_fd, STDERR_FILENO) < 0) {
                close(log_fd);
                _exit(127);
            }
            close(log_fd);
        } else {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }

        struct passwd *pwd = getpwnam(ctx->user);
        struct group *grp = getgrnam(ctx->group);

        if (!pwd || !grp) {
            LOG_ERROR("Service: invalid user/group: %s:%s", ctx->user, ctx->group);
            _exit(127);
        }

        if (initgroups(ctx->user, grp->gr_gid) != 0) {
            LOG_ERROR("Service: initgroups failed: %s", strerror(errno));
            _exit(127);
        }
        if (setgid(grp->gr_gid) != 0) {
            LOG_ERROR("Service: setgid failed: %s", strerror(errno));
            _exit(127);
        }
        if (setuid(pwd->pw_uid) != 0) {
            LOG_ERROR("Service: setuid failed: %s", strerror(errno));
            _exit(127);
        }

        set_service_environment(ctx);

        if (ctx->work_dir[0]) {
            if (chdir(ctx->work_dir) != 0) {
                LOG_WARN("Service: chdir(%s) failed: %s", ctx->work_dir, strerror(errno));
            }
        }

        char **args = build_service_args(ctx);
        if (!args) _exit(127);

        execv(ctx->bin_path, args);
        LOG_ERROR("Service: execv failed: %s", strerror(errno));
        free_service_args(args);
        _exit(127);
    }

    ctx->child_pid = pid;
    ctx->validated_pid = 0;
    LOG_INFO("Service: spawned sing-box (PID: %d)", pid);
    return 0;
}

static void service_kill_timeout_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    kill_state_t *state = userdata;
    service_ctx_t *ctx = state->ctx;

    (void)r;
    (void)timer;

    if (!ctx) {
        free(state);
        return;
    }

    if (!service_is_alive(ctx)) {
        free(state);
        return;
    }

    state->attempts++;
    if (state->attempts >= state->max_attempts) {
        LOG_WARN("Service: sing-box did not respond to SIGTERM, sending SIGKILL");
        kill(ctx->child_pid, SIGKILL);
        free(state);
        return;
    }

    reactor_add_timer(r, 100, 0, service_kill_timeout_cb, state);
}

static void service_kill(service_ctx_t *ctx) {
    if (!ctx || ctx->child_pid <= 0) return;

    LOG_INFO("Service: stopping sing-box (PID: %d)", ctx->child_pid);
    kill(ctx->child_pid, SIGTERM);

    if (!ctx->reactor) {
        LOG_WARN("Service: reactor not available, using blocking fallback");
        usleep(500000);
        if (kill(ctx->child_pid, 0) == 0) {
            kill(ctx->child_pid, SIGKILL);
        }
        waitpid(ctx->child_pid, NULL, WNOHANG);
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        return;
    }

    kill_state_t *state = calloc(1, sizeof(kill_state_t));
    if (!state) {
        LOG_WARN("Service: failed to allocate kill state, using blocking fallback");
        usleep(500000);
        if (kill(ctx->child_pid, 0) == 0) {
            kill(ctx->child_pid, SIGKILL);
        }
        waitpid(ctx->child_pid, NULL, WNOHANG);
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        return;
    }

    state->ctx = ctx;
    state->attempts = 0;
    state->max_attempts = ctx->stop_timeout_sec * 10;

    reactor_add_timer(ctx->reactor, 100, 0, service_kill_timeout_cb, state);
}

static void service_stop_wait_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    service_stop_state_t *state = userdata;
    service_ctx_t *ctx = state->ctx;

    (void)r;
    (void)timer;

    if (!ctx) {
        free(state);
        return;
    }

    if (!service_is_alive(ctx)) {
        LOG_INFO("Service: stopped successfully");
        ctx->state = SERVICE_STOPPED;
        ctx->running_healthy = 0;
        if (state->done_cb) {
            state->done_cb(ctx, state->userdata);
        }
        free(state);
        return;
    }

    state->attempts++;
    if (state->attempts >= state->max_attempts) {
        LOG_ERROR("Service: stop timeout, forcing kill");
        kill(ctx->child_pid, SIGKILL);
        waitpid(ctx->child_pid, NULL, WNOHANG);
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        ctx->state = SERVICE_STOPPED;
        ctx->running_healthy = 0;
        if (state->done_cb) {
            state->done_cb(ctx, state->userdata);
        }
        free(state);
        return;
    }

    reactor_add_timer(r, 100, 0, service_stop_wait_cb, state);
}

static void service_delayed_spawn_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;

    service_ctx_t *ctx = userdata;
    if (!ctx || ctx->state == SERVICE_STOPPED || ctx->state == SERVICE_FAILED) return;

    if (ctx->retry_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
        ctx->retry_timer = NULL;
    }

    if (!circuit_breaker_should_allow(&ctx->breaker)) {
        LOG_WARN("Circuit breaker: open, delaying retry");
        service_schedule_retry(ctx);
        return;
    }

    if (service_spawn(ctx) == 0) {
        ctx->state = SERVICE_STARTING;
    } else {
        ctx->fail_count++;
        circuit_breaker_record_failure(&ctx->breaker);
        if (ctx->fail_count >= ctx->max_failures) {
            LOG_ERROR("Service: max failures reached, giving up");
            ctx->state = SERVICE_FAILED;
        } else {
            service_schedule_retry(ctx);
        }
    }
}

void service_schedule_retry(service_ctx_t *ctx) {
    int delay = backoff_next(&ctx->backoff);
    LOG_INFO("Service: retry in %dms (%d/%d)",
             delay, ctx->fail_count, ctx->max_failures);

    if (ctx->retry_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
        ctx->retry_timer = NULL;
    }
    ctx->retry_timer = reactor_add_timer(ctx->reactor, delay, 0,
                                          service_delayed_spawn_cb, ctx);
}

void service_sigchld_cb(reactor_t *r, int signo, void *userdata) {
    (void)r;
    (void)signo;

    service_ctx_t *ctx = userdata;
    if (!ctx) return;

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == ctx->child_pid) {
            if (WIFEXITED(status)) {
                LOG_WARN("Service: sing-box exited with code %d", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                LOG_WARN("Service: sing-box killed by signal %d", WTERMSIG(status));
            }

            ctx->child_pid = -1;
            ctx->validated_pid = 0;
            ctx->running_healthy = 0;

            if (ctx->state == SERVICE_RUNNING || ctx->state == SERVICE_STARTING) {
                ctx->state = SERVICE_FAILED;
                circuit_breaker_record_failure(&ctx->breaker);
                LOG_INFO("Service: state -> FAILED, will restart on next monitor tick");
            }
        }
    }
}

const char *service_state_string(service_state_t state) {
    switch (state) {
        case SERVICE_STOPPED:  return "STOPPED";
        case SERVICE_STARTING: return "STARTING";
        case SERVICE_RUNNING:  return "RUNNING";
        case SERVICE_FAILED:   return "FAILED";
        case SERVICE_STOPPING: return "STOPPING";
        default:               return "UNKNOWN";
    }
}

void service_health_check_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;

    service_ctx_t *ctx = userdata;
    if (!ctx || ctx->state != SERVICE_RUNNING) return;

    if (!service_is_alive(ctx)) {
        ctx->running_healthy = 0;
        return;
    }

    int healthy = service_api_health_check(ctx);
    ctx->running_healthy = healthy;
    ctx->last_health_check = time(NULL);

    if (!healthy) {
        LOG_DEBUG("Service: health check failed");
        circuit_breaker_record_failure(&ctx->breaker);
    } else {
        circuit_breaker_record_success(&ctx->breaker);
    }
}

void service_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;

    service_ctx_t *ctx = userdata;
    if (!ctx) return;

    switch (ctx->state) {
        case SERVICE_STOPPED:
            break;

        case SERVICE_STARTING:
            if (service_is_alive(ctx)) {
                if (!ctx->validated_pid) {
                    if (validate_process(ctx, ctx->child_pid)) {
                        ctx->validated_pid = 1;
                    } else {
                        LOG_WARN("Service: PID %d validation failed, killing", ctx->child_pid);
                        kill(ctx->child_pid, SIGKILL);
                        ctx->child_pid = -1;
                    }
                }

                if (ctx->validated_pid && service_probe_port(ctx->api_port)) {
                    LOG_INFO("Service: ready on port %d", ctx->api_port);
                    ctx->state = SERVICE_RUNNING;
                    ctx->fail_count = 0;
                    ctx->running_healthy = 1;
                    ctx->last_health_check = time(NULL);
                    backoff_reset(&ctx->backoff);
                    circuit_breaker_record_success(&ctx->breaker);

                    if (ctx->reactor && ctx->health_check_interval_ms > 0) {
                        if (ctx->health_timer) {
                            reactor_cancel_timer(ctx->reactor, ctx->health_timer);
                            ctx->health_timer = NULL;
                        }
                        ctx->health_timer = reactor_add_timer(
                            ctx->reactor, ctx->health_check_interval_ms,
                            ctx->health_check_interval_ms,
                            service_health_check_cb, ctx);
                    }
                }
            } else {
                LOG_WARN("Service: process died during startup");
                ctx->fail_count++;
                circuit_breaker_record_failure(&ctx->breaker);
                if (ctx->fail_count >= ctx->max_failures) {
                    LOG_ERROR("Service: max failures reached, giving up");
                    ctx->state = SERVICE_FAILED;
                } else {
                    service_schedule_retry(ctx);
                }
            }
            break;

        case SERVICE_RUNNING:
            if (!service_is_alive(ctx)) {
                LOG_WARN("Service: process died unexpectedly");
                ctx->state = SERVICE_FAILED;
                ctx->fail_count++;
                circuit_breaker_record_failure(&ctx->breaker);
                if (ctx->fail_count >= ctx->max_failures) {
                    LOG_ERROR("Service: max failures reached, giving up");
                    ctx->state = SERVICE_FAILED;
                } else {
                    service_schedule_retry(ctx);
                }
            }
            break;

        case SERVICE_FAILED:
            break;

        case SERVICE_STOPPING:
            break;
    }
}

int service_init(service_ctx_t *ctx, atp_config_t *cfg) {
    if (!ctx || !cfg) return -1;
    memset(ctx, 0, sizeof(service_ctx_t));

    const char *base_dir = cfg->core.data_dir[0] ? cfg->core.data_dir : ".";

    /* 1. Base directory is the working directory */
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "%s", base_dir);

    /* 2. Locate sing-box binary (candidate: base_dir/bin/sing-box -> base_dir/sing-box) */
    char candidate[PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s/bin/%s", base_dir, PROXY_BIN_NAME);
    if (access(candidate, X_OK) == 0) {
        snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%s", candidate);
    } else {
        snprintf(candidate, sizeof(candidate), "%s/%s", base_dir, PROXY_BIN_NAME);
        if (access(candidate, X_OK) == 0) {
            snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%s", candidate);
        } else {
            snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%s/bin/%s", base_dir, PROXY_BIN_NAME);
        }
    }

    /* 3. Locate configuration (candidate: base_dir/config.json -> base_dir/sing-box.json -> base_dir/sing-box/config.json) */
    snprintf(candidate, sizeof(candidate), "%s/config.json", base_dir);
    if (access(candidate, R_OK) == 0) {
        snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%s", candidate);
    } else {
        snprintf(candidate, sizeof(candidate), "%s/sing-box.json", base_dir);
        if (access(candidate, R_OK) == 0) {
            snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%s", candidate);
        } else {
            snprintf(candidate, sizeof(candidate), "%s/sing-box/config.json", base_dir);
            if (access(candidate, R_OK) == 0) {
                snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%s", candidate);
            } else {
                snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%s/config.json", base_dir);
            }
        }
    }

    /* 4. Logs and user/group */
    snprintf(ctx->log_path, sizeof(ctx->log_path), "%s/run/sing-box.log", base_dir);
    snprintf(ctx->user, sizeof(ctx->user), "%.63s", cfg->core.core_user);
    snprintf(ctx->group, sizeof(ctx->group), "%.63s", cfg->core.core_group);

    ctx->api_port = cfg->api.port;
    ctx->child_pid = -1;
    ctx->validated_pid = 0;
    ctx->state = SERVICE_STOPPED;
    ctx->fail_count = 0;
    ctx->max_failures = cfg->service.max_failures > 0 ? cfg->service.max_failures : 5;
    ctx->start_timeout_sec = cfg->service.start_timeout_sec > 0 ? cfg->service.start_timeout_sec : 30;
    ctx->stop_timeout_sec = cfg->service.stop_timeout_sec > 0 ? cfg->service.stop_timeout_sec : 10;
    ctx->grace_period_sec = cfg->service.grace_period_sec > 0 ? cfg->service.grace_period_sec : 3;
    ctx->health_check_interval_ms = cfg->service.health_check_interval_ms > 0 ? cfg->service.health_check_interval_ms : 5000;
    ctx->stop_attempts = 0;
    ctx->running_healthy = 0;
    ctx->last_health_check = 0;
    ctx->retry_timer = NULL;
    ctx->health_timer = NULL;

    if (cfg->service.args[0]) {
        snprintf(ctx->service_args, sizeof(ctx->service_args), "%s", cfg->service.args);
    }
    if (cfg->service.env[0]) {
        snprintf(ctx->service_env, sizeof(ctx->service_env), "%s", cfg->service.env);
    }

    backoff_init(&ctx->backoff);

    ctx->breaker.threshold = cfg->service.circuit_threshold > 0 ? cfg->service.circuit_threshold : 5;
    ctx->breaker.cooldown_seconds = cfg->service.circuit_cooldown_sec > 0 ? cfg->service.circuit_cooldown_sec : 60;
    circuit_breaker_reset(&ctx->breaker);

    LOG_DEBUG("Service: initialized (bin=%s, port=%d, timeout=%ds, max_fail=%d)",
              ctx->bin_path, ctx->api_port, ctx->start_timeout_sec, ctx->max_failures);
    return 0;
}

int service_set_reactor(service_ctx_t *ctx, reactor_t *r) {
    if (!ctx) return -1;
    ctx->reactor = r;
    if (r) {
        if (ctx->monitor_timer) {
            reactor_cancel_timer(r, ctx->monitor_timer);
            ctx->monitor_timer = NULL;
        }
        ctx->monitor_timer = reactor_add_timer(r, 1000, 1000, service_monitor_cb, ctx);
    }
    return 0;
}

static void on_validate_complete(int result, const char *output, void *userdata) {
    service_ctx_t *ctx = userdata;

    if (result) {
        LOG_INFO("Service: config validation passed");
        if (!circuit_breaker_should_allow(&ctx->breaker)) {
            LOG_WARN("Circuit breaker: open, delaying start");
            service_schedule_retry(ctx);
            return;
        }
        if (service_spawn(ctx) == 0) {
            ctx->state = SERVICE_STARTING;
            ctx->fail_count = 0;
            backoff_reset(&ctx->backoff);
        } else {
            ctx->state = SERVICE_FAILED;
            circuit_breaker_record_failure(&ctx->breaker);
        }
    } else {
        LOG_ERROR("Service: config validation failed: %s", output ? output : "unknown error");
        ctx->state = SERVICE_FAILED;
        circuit_breaker_record_failure(&ctx->breaker);
    }

    free(ctx->validate_ctx);
    ctx->validate_ctx = NULL;
}

int service_start_async(service_ctx_t *ctx) {
    if (!ctx) return -1;

    if (ctx->state == SERVICE_RUNNING || ctx->state == SERVICE_STARTING) {
        LOG_WARN("Service: already running or starting");
        return 0;
    }

    if (!circuit_breaker_should_allow(&ctx->breaker)) {
        LOG_WARN("Service: circuit breaker open, refusing to start");
        return -1;
    }

    if (service_is_alive(ctx)) {
        LOG_INFO("Service: process already running (PID: %d)", ctx->child_pid);
        ctx->state = SERVICE_STARTING;
        return 0;
    }

    if (!service_binary_exists(ctx)) {
        LOG_ERROR("Service: binary not found: %s", ctx->bin_path);
        ctx->state = SERVICE_FAILED;
        return -1;
    }

    ctx->validate_ctx = malloc(sizeof(async_validate_ctx_t));
    if (!ctx->validate_ctx) {
        LOG_ERROR("Service: failed to allocate validate_ctx");
        ctx->state = SERVICE_FAILED;
        return -1;
    }

    int ret = async_validate_config(ctx->validate_ctx, ctx->reactor,
                                    ctx->bin_path, ctx->conf_path, ctx->work_dir,
                                    on_validate_complete, ctx);
    if (ret < 0) {
        LOG_ERROR("Service: failed to start async validation");
        free(ctx->validate_ctx);
        ctx->validate_ctx = NULL;
        ctx->state = SERVICE_FAILED;
        return -1;
    }

    return 0;
}

int service_stop_async(service_ctx_t *ctx, void (*done_cb)(service_ctx_t *, void *), void *userdata) {
    if (!ctx) return -1;

    ctx->state = SERVICE_STOPPING;

    if (ctx->reactor && ctx->health_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->health_timer);
        ctx->health_timer = NULL;
    }

    service_kill(ctx);

    if (!ctx->reactor) {
        ctx->state = SERVICE_STOPPED;
        ctx->fail_count = 0;
        ctx->running_healthy = 0;
        if (done_cb) {
            done_cb(ctx, userdata);
        }
        return 0;
    }

    if (ctx->monitor_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->monitor_timer);
        ctx->monitor_timer = NULL;
    }
    if (ctx->retry_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
        ctx->retry_timer = NULL;
    }

    service_stop_state_t *state = calloc(1, sizeof(service_stop_state_t));
    if (!state) {
        LOG_WARN("Service: failed to allocate stop state, using blocking fallback");
        usleep(500000);
        ctx->state = SERVICE_STOPPED;
        ctx->fail_count = 0;
        ctx->running_healthy = 0;
        if (done_cb) {
            done_cb(ctx, userdata);
        }
        return -1;
    }

    state->ctx = ctx;
    state->attempts = 0;
    state->max_attempts = ctx->stop_timeout_sec * 10;
    state->done_cb = done_cb;
    state->userdata = userdata;

    reactor_add_timer(ctx->reactor, 100, 0, service_stop_wait_cb, state);

    return 0;
}

int service_get_pid(service_ctx_t *ctx) {
    if (!ctx) return -1;

    if (ctx->child_pid > 0) {
        return ctx->child_pid;
    }

    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "%s", ctx->log_path);
    char *dot = strrchr(pid_path, '.');
    if (dot) {
        snprintf(dot, sizeof(pid_path) - (dot - pid_path), ".pid");
    } else {
        const char *base_dir = ctx->work_dir[0] ? ctx->work_dir : ".";
        snprintf(pid_path, sizeof(pid_path), "%s/run/sing-box.pid", base_dir);
    }

    FILE *f = fopen(pid_path, "r");
    if (f) {
        int pid;
        if (fscanf(f, "%d", &pid) == 1 && pid > 0) {
            if (validate_process(ctx, pid)) {
                fclose(f);
                ctx->child_pid = pid;
                ctx->validated_pid = 1;
                return pid;
            } else {
                LOG_WARN("Service: stale PID file");
            }
        }
        fclose(f);
    }

    return -1;
}

int service_is_running(service_ctx_t *ctx) {
    return ctx && ctx->state == SERVICE_RUNNING && service_is_alive(ctx);
}

int service_is_healthy(service_ctx_t *ctx) {
    return ctx && service_is_running(ctx) && ctx->running_healthy;
}
