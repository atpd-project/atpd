/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Service Manager - Industrial-grade async state machine
 * Features: PID validation, SIGCHLD integration, exponential backoff
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include "async_validate.h"

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

static int validate_process(service_ctx_t *ctx, pid_t pid) {
    char path[256];
    char exe_buf[256];
    char cmdline_buf[512];
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

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    len = read(fd, cmdline_buf, sizeof(cmdline_buf) - 1);
    close(fd);

    if (len > 0) {
        cmdline_buf[len] = '\0';
        if (!strstr(cmdline_buf, ctx->work_dir)) {
            LOG_WARN("Service: PID %d cmdline missing work_dir", pid);
            return 0;
        }
    }

    LOG_INFO("Service: PID %d validated successfully", pid);
    return 1;
}

static int service_is_alive(service_ctx_t *ctx) {
    if (ctx->child_pid <= 0) return 0;
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
    close(sock);

    if (ret == 0) return 1;
    return (errno == EINPROGRESS) ? 1 : 0;
}

static int service_binary_exists(service_ctx_t *ctx) {
    return access(ctx->bin_path, X_OK) == 0;
}

static void service_rotate_log(service_ctx_t *ctx) {
    struct stat st;
    char backup_path[512];
    char timestamp[64];
    char cmd[1024];
    time_t now;
    struct tm *tm_info;

    if (stat(ctx->log_path, &st) != 0) {
        return;
    }

    if (st.st_size < MAX_LOG_SIZE) {
        return;
    }

    now = time(NULL);
    tm_info = localtime(&now);
    if (!tm_info) {
        LOG_WARN("Service: failed to get localtime for log rotation");
        return;
    }

    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm_info);
    snprintf(backup_path, sizeof(backup_path), "%s.%s", ctx->log_path, timestamp);

    if (rename(ctx->log_path, backup_path) == 0) {
        snprintf(cmd, sizeof(cmd), "busybox gzip -9 %s 2>/dev/null || true", backup_path);
        exec_cmd_simple(cmd, 5);
        LOG_INFO("Service: rotated and compressed log to %s.gz (size: %ld bytes)",
                 backup_path, (long)st.st_size);
    } else {
        LOG_WARN("Service: failed to rotate log: %s", strerror(errno));
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
        umask(027);
        setsid();
        signal(SIGPIPE, SIG_IGN);

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        int log_fd = open(ctx->log_path, O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
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
        if (grp) setgid(grp->gr_gid);
        if (pwd) setuid(pwd->pw_uid);

        char *argv[] = { ctx->bin_path, "run", "-D", ctx->work_dir, NULL };
        execv(ctx->bin_path, argv);
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
    int status;
    pid_t result;

    (void)r;
    (void)timer;

    if (!ctx) {
        free(state);
        return;
    }

    result = waitpid(ctx->child_pid, &status, WNOHANG);

    if (result == ctx->child_pid) {
        if (WIFEXITED(status)) {
            LOG_DEBUG("Service: sing-box exited with code %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOG_DEBUG("Service: sing-box killed by signal %d", WTERMSIG(status));
        }
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        free(state);
        return;
    }

    if (kill(ctx->child_pid, 0) != 0) {
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        free(state);
        return;
    }

    state->attempts++;
    if (state->attempts >= state->max_attempts) {
        LOG_WARN("Service: sing-box did not respond to SIGTERM, sending SIGKILL");
        kill(ctx->child_pid, SIGKILL);
        waitpid(ctx->child_pid, NULL, WNOHANG);
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
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
    state->max_attempts = 20;

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
        if (state->done_cb) {
            state->done_cb(ctx, state->userdata);
        }
        free(state);
        return;
    }

    state->attempts++;
    if (state->attempts >= state->max_attempts) {
        LOG_WARN("Service: stop timeout, forcing kill");
        kill(ctx->child_pid, SIGKILL);
        waitpid(ctx->child_pid, NULL, WNOHANG);
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        ctx->state = SERVICE_STOPPED;
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

    reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
    ctx->retry_timer = NULL;

    if (service_spawn(ctx) == 0) {
        ctx->state = SERVICE_STARTING;
    } else {
        ctx->fail_count++;
        if (ctx->fail_count >= ctx->max_failures) {
            LOG_ERROR("Service: max failures reached, giving up");
            ctx->state = SERVICE_FAILED;
        }
    }
}

static void service_schedule_retry(service_ctx_t *ctx) {
    int delay = backoff_next(&ctx->backoff);
    LOG_INFO("Service: retry in %dms (%d/%d)",
             delay, ctx->fail_count, ctx->max_failures);

    if (ctx->retry_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
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

            if (ctx->state == SERVICE_RUNNING || ctx->state == SERVICE_STARTING) {
                ctx->state = SERVICE_FAILED;
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
                    backoff_reset(&ctx->backoff);
                }
            } else {
                LOG_WARN("Service: process died during startup");
                ctx->fail_count++;
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
                ctx->fail_count++;
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

    snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%.4000s/bin/%.63s",
             cfg->data_dir, PROXY_BIN_NAME);
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "%.4000s/sing-box", cfg->data_dir);
    snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%.4000s/sing-box/config.json", cfg->data_dir);
    snprintf(ctx->log_path, sizeof(ctx->log_path), "%.4000s/run/sing-box.log", cfg->data_dir);
    snprintf(ctx->user, sizeof(ctx->user), "%.63s", cfg->core_user);
    snprintf(ctx->group, sizeof(ctx->group), "%.63s", cfg->core_group);

    ctx->api_port = cfg->api_port;
    ctx->child_pid = -1;
    ctx->validated_pid = 0;
    ctx->state = SERVICE_STOPPED;
    ctx->fail_count = 0;
    ctx->max_failures = 5;
    ctx->retry_timer = NULL;

    backoff_init(&ctx->backoff);

    LOG_DEBUG("Service: initialized (bin=%s, port=%d)", ctx->bin_path, ctx->api_port);
    return 0;
}

static void on_validate_complete(int result, const char *output, void *userdata) {
    service_ctx_t *ctx = userdata;

    if (result) {
        LOG_INFO("Service: config validation passed");
        if (service_spawn(ctx) == 0) {
            ctx->state = SERVICE_STARTING;
            ctx->fail_count = 0;
            backoff_reset(&ctx->backoff);
        } else {
            ctx->state = SERVICE_FAILED;
        }
    } else {
        LOG_ERROR("Service: config validation failed: %s", output ? output : "unknown error");
        ctx->state = SERVICE_FAILED;
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
                                    ctx->bin_path, ctx->work_dir,
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

    service_kill(ctx);

    if (!ctx->reactor) {
        ctx->state = SERVICE_STOPPED;
        ctx->fail_count = 0;
        if (ctx->monitor_timer) {
            reactor_cancel_timer(ctx->reactor, ctx->monitor_timer);
            ctx->monitor_timer = NULL;
        }
        if (ctx->retry_timer) {
            reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
            ctx->retry_timer = NULL;
        }
        if (done_cb) {
            done_cb(ctx, userdata);
        }
        return 0;
    }

    service_stop_state_t *state = calloc(1, sizeof(service_stop_state_t));
    if (!state) {
        LOG_WARN("Service: failed to allocate stop state, using blocking fallback");
        usleep(500000);
        ctx->state = SERVICE_STOPPED;
        ctx->fail_count = 0;
        if (ctx->monitor_timer) {
            reactor_cancel_timer(ctx->reactor, ctx->monitor_timer);
            ctx->monitor_timer = NULL;
        }
        if (ctx->retry_timer) {
            reactor_cancel_timer(ctx->reactor, ctx->retry_timer);
            ctx->retry_timer = NULL;
        }
        if (done_cb) {
            done_cb(ctx, userdata);
        }
        return 0;
    }

    state->ctx = ctx;
    state->attempts = 0;
    state->max_attempts = 50;
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

    char pid_path[256];
    snprintf(pid_path, sizeof(pid_path), "%s/run/sing-box.pid", ATP_DEFAULT_DIR);

    FILE *f = fopen(pid_path, "r");
    if (f) {
        int pid;
        if (fscanf(f, "%d", &pid) == 1 && pid > 0) {
            fclose(f);
            ctx->child_pid = pid;
            return pid;
        }
        fclose(f);
    }

    return -1;
}

int service_is_running(service_ctx_t *ctx) {
    return ctx && ctx->state == SERVICE_RUNNING && service_is_alive(ctx);
}
