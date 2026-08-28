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
#include <stdarg.h>
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
#include <libgen.h>
#include "async_validate.h"
#include "netlink.h"

/* Forward declarations */
void service_schedule_retry(service_ctx_t *ctx);

#define MAX_LOG_SIZE (10 * 1024 * 1024)

static int open_regular_file(const char *path, int flags, mode_t mode) {
    int fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

static int format_path(char *path, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int length = vsnprintf(path, size, format, args);
    va_end(args);
    return length >= 0 && (size_t)length < size ? 0 : -1;
}

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
    if (kill(pid, 0) != 0) return 0;

    char path[PATH_MAX];
    char exe_buf[PATH_MAX];
    ssize_t len;

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    len = readlink(path, exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        char exe_copy[PATH_MAX];
        strncpy(exe_copy, exe_buf, sizeof(exe_copy) - 1);
        exe_copy[sizeof(exe_copy) - 1] = '\0';

        char bin_copy[PATH_MAX];
        strncpy(bin_copy, ctx->bin_path, sizeof(bin_copy) - 1);
        bin_copy[sizeof(bin_copy) - 1] = '\0';

        char *b1 = basename(exe_copy);
        char *b2 = basename(bin_copy);

        if (b1 && b2 && strcmp(b1, b2) == 0) {
            return 1;
        }

        if (strstr(exe_buf, PROXY_BIN_NAME) != NULL) {
            return 1;
        }
    }

    /* Fallback to /proc/<pid>/comm */
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char comm[64] = {0};
        if (fgets(comm, sizeof(comm), fp)) {
            trim(comm);
            if (strstr(comm, PROXY_BIN_NAME) != NULL || strcmp(comm, "sing-box") == 0) {
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }

    return 0;
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

void service_pid_path(service_ctx_t *ctx, char *path, size_t size) {
    if (!ctx || !path || size == 0) return;
    snprintf(path, size, "%s", ctx->pid_path);
}

static void service_unlink_pid(service_ctx_t *ctx) {
    char pid_path[PATH_MAX];
    service_pid_path(ctx, pid_path, sizeof(pid_path));
    unlink(pid_path);
}

void service_rotate_log(service_ctx_t *ctx) {
    struct stat st;
    char backup_path[PATH_MAX];
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
    if (format_path(backup_path, sizeof(backup_path), "%s.%s",
                    ctx->log_path, timestamp) != 0) {
        LOG_WARN("Service: rotated log path is too long");
        return;
    }

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

    if (ctx->work_dir[0] && mkdir_recursive(ctx->work_dir, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("Service: failed to create sing-box data directory %s: %s",
                  ctx->work_dir, strerror(errno));
        return -1;
    }

    char pid_path[PATH_MAX];
    service_pid_path(ctx, pid_path, sizeof(pid_path));
    char run_dir[PATH_MAX];
    snprintf(run_dir, sizeof(run_dir), "%s", pid_path);
    char *run_slash = strrchr(run_dir, '/');
    if (run_slash) {
        *run_slash = '\0';
        if (mkdir_recursive(run_dir, 0755) != 0 && errno != EEXIST) {
            LOG_ERROR("Service: failed to create runtime directory %s: %s",
                      run_dir, strerror(errno));
            return -1;
        }
    }

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

        int log_fd = open_regular_file(ctx->log_path,
                                       O_WRONLY | O_CREAT | O_APPEND, 0640);
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

        uid_t target_uid = getuid();
        gid_t target_gid = getgid();

        struct passwd *pwd = NULL;
        if (ctx->user[0]) {
            pwd = getpwnam(ctx->user);
            if (pwd) {
                target_uid = pwd->pw_uid;
                target_gid = pwd->pw_gid;
            } else if (is_number(ctx->user)) {
                errno = 0;
                unsigned long value = strtoul(ctx->user, NULL, 10);
                target_uid = (uid_t)value;
                if (errno == ERANGE || (unsigned long)target_uid != value) {
                    LOG_ERROR("Service: invalid numeric user '%s'", ctx->user);
                    _exit(127);
                }
            } else {
                LOG_ERROR("Service: unknown user '%s'", ctx->user);
                _exit(127);
            }
        }

        if (ctx->group[0]) {
            struct group *grp = getgrnam(ctx->group);
            if (grp) {
                target_gid = grp->gr_gid;
            } else if (is_number(ctx->group)) {
                errno = 0;
                unsigned long value = strtoul(ctx->group, NULL, 10);
                target_gid = (gid_t)value;
                if (errno == ERANGE || (unsigned long)target_gid != value) {
                    LOG_ERROR("Service: invalid numeric group '%s'", ctx->group);
                    _exit(127);
                }
            } else {
                LOG_ERROR("Service: unknown group '%s'", ctx->group);
                _exit(127);
            }
        }

        if (geteuid() == 0) {
            if (pwd) {
                if (initgroups(pwd->pw_name, target_gid) != 0) {
                    LOG_ERROR("Service: initgroups failed: %s", strerror(errno));
                    _exit(127);
                }
            } else if (setgroups(0, NULL) != 0) {
                LOG_ERROR("Service: setgroups failed: %s", strerror(errno));
                _exit(127);
            }
            if (setgid(target_gid) != 0) {
                LOG_ERROR("Service: setgid failed: %s", strerror(errno));
                _exit(127);
            }
            if (setuid(target_uid) != 0) {
                LOG_ERROR("Service: setuid failed: %s", strerror(errno));
                _exit(127);
            }
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
    ctx->validated_pid = 1;
    ctx->start_time = time(NULL);
    LOG_INFO("Service: spawned sing-box (PID: %d)", pid);

    int pid_fd = open_regular_file(pid_path, O_WRONLY | O_CREAT, 0644);
    int pid_file_ok = pid_fd >= 0;
    if (pid_file_ok && ftruncate(pid_fd, 0) != 0) pid_file_ok = 0;
    if (pid_file_ok && dprintf(pid_fd, "%d\n", pid) < 0) pid_file_ok = 0;
    if (pid_file_ok && fsync(pid_fd) != 0) pid_file_ok = 0;
    if (pid_fd >= 0 && close(pid_fd) != 0) pid_file_ok = 0;
    if (!pid_file_ok) {
        LOG_ERROR("Service: failed to write sing-box PID file %s", pid_path);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        unlink(pid_path);
        ctx->child_pid = -1;
        ctx->validated_pid = 0;
        return -1;
    }
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
        service_unlink_pid(ctx);
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
        service_unlink_pid(ctx);
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
    if (!ctx || ctx->state == SERVICE_STOPPED) return;

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
    pid_t pid = ctx->child_pid > 0 ? waitpid(ctx->child_pid, &status, WNOHANG) : 0;
    if (pid == ctx->child_pid) {
            if (WIFEXITED(status)) {
                LOG_WARN("Service: sing-box exited with code %d", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                LOG_WARN("Service: sing-box killed by signal %d", WTERMSIG(status));
            }

            ctx->child_pid = -1;
            ctx->validated_pid = 0;
            ctx->running_healthy = 0;

            char pid_path[PATH_MAX];
            service_pid_path(ctx, pid_path, sizeof(pid_path));
            unlink(pid_path);

            if (ctx->state == SERVICE_RUNNING || ctx->state == SERVICE_STARTING) {
                ctx->state = SERVICE_FAILED;
                circuit_breaker_record_failure(&ctx->breaker);
                if (ctx->reactor && ctx->fail_count < ctx->max_failures) {
                    ctx->fail_count++;
                    LOG_INFO("Service: state -> FAILED, scheduling restart");
                    service_schedule_retry(ctx);
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
            if (ctx->start_time > 0 && time(NULL) - ctx->start_time >= ctx->start_timeout_sec) {
                LOG_WARN("Service: startup timed out after %ds", ctx->start_timeout_sec);
                if (ctx->child_pid > 0) kill(ctx->child_pid, SIGKILL);
                ctx->state = SERVICE_FAILED;
                ctx->fail_count++;
                circuit_breaker_record_failure(&ctx->breaker);
                if (ctx->reactor && ctx->fail_count < ctx->max_failures) service_schedule_retry(ctx);
                break;
            }
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

                    if (ctx->reactor) {
                        netlink_refresh_state(ctx->reactor);
                    }

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
            if (ctx->reactor && ctx->child_pid <= 0 && ctx->fail_count < ctx->max_failures && !ctx->retry_timer) {
                service_schedule_retry(ctx);
            }
            break;

        case SERVICE_STOPPING:
            break;
    }
}

int service_init(service_ctx_t *ctx, atp_config_t *cfg) {
    if (!ctx || !cfg) return -1;
    memset(ctx, 0, sizeof(service_ctx_t));

    const char *base_dir = cfg->core.data_dir[0] ? cfg->core.data_dir : ".";

    /* sing-box uses the installation root as its working directory. */
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "%s", base_dir);

    /* Locate the bundled sing-box binary. */
    char candidate[PATH_MAX];
    if (format_path(candidate, sizeof(candidate), "%s/bin/%s",
                    base_dir, PROXY_BIN_NAME) != 0) {
        LOG_ERROR("Service: sing-box binary path is too long");
        return -1;
    }
    snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%s", candidate);

    /* sing-box configuration lives beside the binary's working directory. */
    if (format_path(ctx->conf_path, sizeof(ctx->conf_path), "%s/config.json", base_dir) != 0) {
        LOG_ERROR("Service: sing-box configuration path is too long");
        return -1;
    }

    /* Logs stay in sing-box's working directory; ATPd runtime files stay in run/. */
    if (format_path(ctx->log_path, sizeof(ctx->log_path), "%s/sing-box.log", base_dir) != 0) {
        LOG_ERROR("Service: sing-box log path is too long");
        return -1;
    }
    const char *run_dir = cfg->core.run_dir[0] ? cfg->core.run_dir : ATP_RUN_DIR;
    if (run_dir[0] == '/') {
        if (format_path(ctx->pid_path, sizeof(ctx->pid_path), "%s/sing-box.pid", run_dir) != 0) {
            LOG_ERROR("Service: sing-box PID path is too long");
            return -1;
        }
    } else {
        if (format_path(ctx->pid_path, sizeof(ctx->pid_path), "%s/%s/sing-box.pid",
                        base_dir, run_dir) != 0) {
            LOG_ERROR("Service: sing-box PID path is too long");
            return -1;
        }
    }
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
    ctx->start_time = 0;
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

int service_apply_config(service_ctx_t *ctx, const atp_config_t *cfg) {
    if (!ctx || !cfg) return -1;
    ctx->api_port = cfg->api.port;
    ctx->max_failures = cfg->service.max_failures > 0 ? cfg->service.max_failures : 5;
    ctx->start_timeout_sec = cfg->service.start_timeout_sec > 0 ? cfg->service.start_timeout_sec : 30;
    ctx->stop_timeout_sec = cfg->service.stop_timeout_sec > 0 ? cfg->service.stop_timeout_sec : 10;
    ctx->grace_period_sec = cfg->service.grace_period_sec > 0 ? cfg->service.grace_period_sec : 3;
    ctx->health_check_interval_ms = cfg->service.health_check_interval_ms > 0 ? cfg->service.health_check_interval_ms : 5000;
    snprintf(ctx->user, sizeof(ctx->user), "%.63s", cfg->core.core_user);
    snprintf(ctx->group, sizeof(ctx->group), "%.63s", cfg->core.core_group);
    snprintf(ctx->service_args, sizeof(ctx->service_args), "%.511s", cfg->service.args);
    snprintf(ctx->service_env, sizeof(ctx->service_env), "%.511s", cfg->service.env);
    if (ctx->reactor && ctx->state == SERVICE_RUNNING && ctx->health_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->health_timer);
        ctx->health_timer = reactor_add_timer(ctx->reactor, ctx->health_check_interval_ms,
                                               ctx->health_check_interval_ms,
                                               service_health_check_cb, ctx);
    }
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

    if (service_spawn(ctx) == 0) {
        ctx->state = SERVICE_STARTING;
        ctx->start_time = time(NULL);
        ctx->fail_count = 0;
        backoff_reset(&ctx->backoff);
        return 0;
    } else {
        ctx->state = SERVICE_FAILED;
        circuit_breaker_record_failure(&ctx->breaker);
        return -1;
    }
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
        service_unlink_pid(ctx);
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
        service_unlink_pid(ctx);
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
    service_pid_path(ctx, pid_path, sizeof(pid_path));

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
