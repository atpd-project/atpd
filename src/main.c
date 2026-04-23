/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Service Manager - Async Reactor-driven state machine
 */

#include "service.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <signal.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)

/* ========== State String ========== */

const char *service_state_string(service_state_t state) {
    switch (state) {
        case SERVICE_STOPPED:      return "STOPPED";
        case SERVICE_RUNNING:      return "RUNNING";
        case SERVICE_STARTING:     return "STARTING";
        case SERVICE_STOPPING:     return "STOPPING";
        case SERVICE_FAILED:       return "FAILED";
        case SERVICE_WAIT_PROCESS: return "WAIT_PROCESS";
        case SERVICE_WAIT_API:     return "WAIT_API";
        default:                   return "UNKNOWN";
    }
}

int service_is_ready(service_ctx_t *ctx) {
    return ctx && ctx->state == SERVICE_RUNNING;
}

time_t service_get_uptime(service_ctx_t *ctx) {
    if (!ctx || ctx->state != SERVICE_RUNNING) return 0;
    return time(NULL) - ctx->start_time;
}

/* ========== Process Utilities ========== */

pid_t service_find_process(const char *name) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    struct dirent *entry;
    pid_t found_pid = -1;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) continue;
        pid_t pid = atoi(entry->d_name);
        char path[SAFE_PATH_MAX], line[256];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE *fp = fopen(path, "r");
        if (fp) {
            if (fgets(line, sizeof(line), fp)) {
                trim(line);
                if (strcmp(line, name) == 0) { 
                    found_pid = pid; 
                    fclose(fp); 
                    break; 
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return found_pid;
}

int service_kill_process(pid_t pid, int signal, int wait_sec) {
    if (pid <= 0) return -1;
    if (kill(pid, signal) != 0) return -1;
    if (wait_sec > 0) wait_for_pid_exit(pid, (unsigned int)wait_sec);
    return 0;
}

int service_kill_all(const char *name, int signal) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    struct dirent *entry; 
    int killed = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) continue;
        pid_t pid = atoi(entry->d_name);
        char path[SAFE_PATH_MAX], line[256];
        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        FILE *fp = fopen(path, "r");
        if (fp) {
            if (fgets(line, sizeof(line), fp)) {
                trim(line);
                if (strcmp(line, name) == 0) { 
                    kill(pid, signal); 
                    killed++; 
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    if (killed > 0) LOG_DEBUG("Killed %d processes with name %s", killed, name);
    return killed;
}
/* ========== PID Lock ========== */

int service_acquire_pid_lock(service_ctx_t *ctx) {
    char dir[SAFE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%.4095s", ctx->pid_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_recursive(dir, 0755);
    }

    ctx->pid_fd = open(ctx->pid_path, O_CREAT | O_RDWR, 0640);
    if (ctx->pid_fd < 0) {
        LOG_ERROR("Failed to open PID file: %s", strerror(errno));
        return -1;
    }

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    if (fcntl(ctx->pid_fd, F_SETLK, &fl) < 0) {
        LOG_ERROR("Another instance is running (locked): %s", strerror(errno));
        close(ctx->pid_fd);
        ctx->pid_fd = -1;
        return -1;
    }

    char pid_str[16];
    int len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    ftruncate(ctx->pid_fd, 0);
    if (write(ctx->pid_fd, pid_str, (size_t)len) < 0) return -1;

    LOG_DEBUG("PID lock acquired: %s", ctx->pid_path);
    return 0;
}

void service_release_pid_lock(service_ctx_t *ctx) {
    if (ctx->pid_fd >= 0) {
        close(ctx->pid_fd);
        ctx->pid_fd = -1;
    }
    if (ctx->pid_path[0] != '\0') unlink(ctx->pid_path);
    LOG_DEBUG("PID lock released");
}

/* ========== Config Validation ========== */

int service_validate_config(service_ctx_t *ctx) {
    char cmd[PATH_MAX * 2 + 128];
    char output[4096];
    snprintf(cmd, sizeof(cmd), "%.2048s check -D \"%.2048s\" 2>&1", ctx->bin_path, ctx->work_dir);
    int ret = exec_cmd(cmd, output, sizeof(output), 10);
    if (ret != 0) {
        LOG_ERROR("Config validation failed: %s", output);
        return -1;
    }
    LOG_DEBUG("Config validation passed");
    return 0;
}

/* ========== Log Rotation ========== */

static int safe_log_rotate(const char *log_path) {
    struct stat st;
    if (stat(log_path, &st) != 0) return 0;
    int test_fd = open(log_path, O_RDONLY | O_NONBLOCK);
    if (test_fd < 0) {
        LOG_WARN("Log file %s is busy, skipping rotation", log_path);
        return -1;
    }
    close(test_fd);
    char old_path[SAFE_PATH_MAX];
    snprintf(old_path, sizeof(old_path), "%.4090s.1", log_path);
    unlink(old_path);
    if (rename(log_path, old_path) != 0) {
        LOG_WARN("Failed to rotate log file: %s", strerror(errno));
        return -1;
    }
    LOG_DEBUG("Log rotated: %s -> %s", log_path, old_path);
    return 0;
}

int service_rotate_log(service_ctx_t *ctx) {
    struct stat st;
    if (stat(ctx->log_path, &st) != 0 || st.st_size < 10 * 1024 * 1024) return 0;
    char old_path[SAFE_PATH_MAX];
    snprintf(old_path, sizeof(old_path), "%.4090s.1", ctx->log_path);
    rename(ctx->log_path, old_path);
    LOG_INFO("Log rotated");
    return 0;
}

/* ========== Cooldown ========== */

void service_set_cooldown(service_ctx_t *ctx, int seconds) {
    ctx->restart_cooldown_sec = seconds;
}

int service_cooldown_active(service_ctx_t *ctx) {
    int elapsed = (int)(time(NULL) - ctx->last_restart_time);
    if (ctx->last_restart_time > 0 && elapsed < ctx->restart_cooldown_sec) {
        LOG_DEBUG("Cooldown active: %d seconds remaining", ctx->restart_cooldown_sec - elapsed);
        return 1;
    }
    return 0;
}

void service_reset_failures(service_ctx_t *ctx) {
    ctx->restart_failures = 0;
    ctx->last_restart_time = time(NULL);
}
/* ========== Permissions ========== */

static int setup_run_directory_permissions(service_ctx_t *ctx) {
    char run_dir[SAFE_PATH_MAX];
    const char *base = (ctx->work_dir[0] != '\0') ? ctx->work_dir : ATP_DEFAULT_DIR;
    snprintf(run_dir, sizeof(run_dir), "%.4090s/run", base);
    struct passwd *pwd = getpwnam(ctx->user);
    struct group *grp = getgrnam(ctx->group);
    if (!pwd || !grp) return -1;
    if (chown(run_dir, pwd->pw_uid, grp->gr_gid) != 0) return -1;
    if (chmod(run_dir, 0750) != 0) return -1;
    return 0;
}

static int setup_work_directory_permissions(service_ctx_t *ctx) {
    if (ctx->work_dir[0] == '\0') return -1;
    mkdir_recursive(ctx->work_dir, 0755);
    struct passwd *pwd = getpwnam(ctx->user);
    struct group *grp = getgrnam(ctx->group);
    if (!pwd || !grp) return -1;
    if (chown(ctx->work_dir, pwd->pw_uid, grp->gr_gid) != 0) return -1;
    if (chmod(ctx->work_dir, 0750) != 0) return -1;
    return 0;
}

static int set_user_group(service_ctx_t *ctx) {
    struct group *grp = getgrnam(ctx->group);
    if (grp != NULL) {
        if (setgid(grp->gr_gid) != 0) return -1;
    }
    struct passwd *pwd = getpwnam(ctx->user);
    if (pwd != NULL) {
        if (setuid(pwd->pw_uid) != 0) return -1;
    }
    return 0;
}

/* ========== API Ready Check ========== */

static int check_api_ready(const char *addr, int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(sock);
        return 0;
    }
    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    close(sock);
    return ret == 0;
}

int service_check_port(int port) {
    return check_api_ready("127.0.0.1", port);
}
/* ========== Init ========== */

int service_init(service_ctx_t *ctx, atp_config_t *cfg) {
    if (!ctx || !cfg) return -1;
    memset(ctx, 0, sizeof(service_ctx_t));
    ctx->pid_fd = -1;
    ctx->target_pid = -1;

    snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%.4000s/bin/%.63s", cfg->data_dir, PROXY_BIN_NAME);
    snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%.4000s/sing-box/config.json", cfg->data_dir);
    snprintf(ctx->log_path, sizeof(ctx->log_path), "%.4000s/run/sing-box.log", cfg->data_dir);
    snprintf(ctx->pid_path, sizeof(ctx->pid_path), "%.4000s/run/sing-box.pid", cfg->data_dir);
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "%.4000s/sing-box", cfg->data_dir);
    snprintf(ctx->user, sizeof(ctx->user), "%.63s", cfg->core_user);
    snprintf(ctx->group, sizeof(ctx->group), "%.63s", cfg->core_group);
    snprintf(ctx->api_addr, sizeof(ctx->api_addr), "127.0.0.1");
    ctx->api_port = cfg->api_port;

    ctx->restart_cooldown_sec = 60;
    ctx->restart_delay_sec = (cfg->restart_delay <= 0) ? DEFAULT_RESTART_DELAY : cfg->restart_delay;
    ctx->state = SERVICE_STOPPED;

    LOG_DEBUG("Service initialized: bin=%s", ctx->bin_path);
    return 0;
}

/* ========== State Machine Callbacks ========== */

static void service_transition(service_ctx_t *ctx, service_state_t new_state) {
    LOG_DEBUG("Service: %s -> %s", service_state_string(ctx->state), service_state_string(new_state));
    ctx->state = new_state;
}

static void service_cleanup_timers(service_ctx_t *ctx) {
    if (ctx->wait_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->wait_timer);
        ctx->wait_timer = NULL;
    }
    if (ctx->restart_timer) {
        reactor_cancel_timer(ctx->reactor, ctx->restart_timer);
        ctx->restart_timer = NULL;
    }
}

static void wait_api_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);

static void wait_process_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    service_ctx_t *ctx = userdata;
    (void)r;
    (void)timer;

    ctx->wait_timer = NULL;
    if (ctx->state != SERVICE_WAIT_PROCESS) return;

    pid_t found = service_find_process(PROXY_BIN_NAME);
    if (found > 0) {
        ctx->target_pid = found;
        ctx->start_time = time(NULL);
        LOG_INFO("Service: sing-box process found (PID: %d)", found);
        service_transition(ctx, SERVICE_WAIT_API);
        ctx->wait_timer = reactor_add_timer(ctx->reactor, 200, 200, wait_api_cb, ctx);
        return;
    }

    if (time(NULL) - ctx->start_time > 10) {
        LOG_ERROR("Service: timeout waiting for process");
        service_transition(ctx, SERVICE_FAILED);
        service_cleanup_timers(ctx);
        if (ctx->on_error) ctx->on_error(ctx, -1, "Process timeout", ctx->callback_userdata);
        return;
    }

    ctx->wait_timer = reactor_add_timer(ctx->reactor, 200, 0, wait_process_cb, ctx);
}

static void wait_api_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    service_ctx_t *ctx = userdata;
    (void)r;
    (void)timer;

    ctx->wait_timer = NULL;
    if (ctx->state != SERVICE_WAIT_API) return;

    if (!process_exists(ctx->target_pid)) {
        LOG_WARN("Service: process died during startup");
        ctx->target_pid = -1;
        if (ctx->restart_failures < 3) {
            ctx->restart_failures++;
            LOG_INFO("Service: restarting (%d/3)", ctx->restart_failures);
            service_transition(ctx, SERVICE_STARTING);
            ctx->restart_timer = reactor_add_timer(ctx->reactor, ctx->restart_delay_sec * 1000, 0, wait_process_cb, ctx);
        } else {
            LOG_ERROR("Service: max restart attempts");
            service_transition(ctx, SERVICE_FAILED);
            if (ctx->on_error) ctx->on_error(ctx, -1, "Max restart attempts", ctx->callback_userdata);
        }
        return;
    }

    if (check_api_ready(ctx->api_addr, ctx->api_port)) {
        LOG_INFO("Service: API ready at %s:%d", ctx->api_addr, ctx->api_port);
        service_transition(ctx, SERVICE_RUNNING);
        service_cleanup_timers(ctx);
        ctx->restart_failures = 0;
        if (ctx->on_ready) ctx->on_ready(ctx, ctx->callback_userdata);
        return;
    }

    if (time(NULL) - ctx->start_time > 10) {
        LOG_ERROR("Service: timeout waiting for API");
        service_transition(ctx, SERVICE_FAILED);
        service_cleanup_timers(ctx);
        if (ctx->on_error) ctx->on_error(ctx, -1, "API timeout", ctx->callback_userdata);
        return;
    }

    ctx->wait_timer = reactor_add_timer(ctx->reactor, 200, 0, wait_api_cb, ctx);
}

static void restart_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    service_ctx_t *ctx = userdata;
    (void)r;
    (void)timer;

    ctx->restart_timer = NULL;
    if (ctx->state != SERVICE_STARTING) return;

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("Service: fork failed: %s", strerror(errno));
        service_transition(ctx, SERVICE_FAILED);
        if (ctx->on_error) ctx->on_error(ctx, errno, "Fork failed", ctx->callback_userdata);
        return;
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

        set_user_group(ctx);
        char *argv[] = { (char *)ctx->bin_path, "run", "-D", (char *)ctx->work_dir, NULL };
        execv(ctx->bin_path, argv);
        _exit(127);
    }

    ctx->target_pid = pid;
    ctx->start_time = time(NULL);
    LOG_INFO("Service: spawned sing-box (PID: %d)", pid);

    service_transition(ctx, SERVICE_WAIT_PROCESS);
    ctx->wait_timer = reactor_add_timer(ctx->reactor, 100, 0, wait_process_cb, ctx);
}

/* ========== Public API ========== */

int service_start_async(service_ctx_t *ctx, reactor_t *r,
                        service_ready_cb on_ready,
                        service_error_cb on_error,
                        void *userdata) {
    if (!ctx || !r) return -1;

    if (ctx->state != SERVICE_STOPPED && ctx->state != SERVICE_FAILED) {
        LOG_WARN("Service: already starting or running");
        return -1;
    }

    if (service_acquire_pid_lock(ctx) != 0) {
        LOG_ERROR("Service: failed to acquire PID lock");
        return -1;
    }

    if (service_validate_config(ctx) != 0) {
        LOG_ERROR("Service: config validation failed");
        service_release_pid_lock(ctx);
        return -1;
    }

    setup_run_directory_permissions(ctx);
    setup_work_directory_permissions(ctx);
    safe_log_rotate(ctx->log_path);
    service_kill_all(PROXY_BIN_NAME, SIGKILL);

    ctx->reactor = r;
    ctx->on_ready = on_ready;
    ctx->on_error = on_error;
    ctx->callback_userdata = userdata;
    ctx->restart_failures = 0;

    service_transition(ctx, SERVICE_STARTING);
    ctx->restart_timer = reactor_add_timer(r, 100, 0, restart_timer_cb, ctx);

    LOG_INFO("Service: async start initiated");
    return 0;
}

int service_stop_async(service_ctx_t *ctx) {
    if (!ctx) return -1;
    service_cleanup_timers(ctx);
    if (ctx->target_pid > 0) {
        LOG_INFO("Service: stopping sing-box (PID: %d)", ctx->target_pid);
        kill(ctx->target_pid, SIGTERM);
        ctx->target_pid = -1;
    }
    service_kill_all(PROXY_BIN_NAME, SIGKILL);
    service_release_pid_lock(ctx);
    service_transition(ctx, SERVICE_STOPPED);
    return 0;
}

int service_restart_async(service_ctx_t *ctx) {
    if (!ctx) return -1;
    LOG_INFO("Service: async restart requested");
    service_stop_async(ctx);
    ctx->restart_timer = reactor_add_timer(ctx->reactor, ctx->restart_delay_sec * 1000, 0, restart_timer_cb, ctx);
    service_transition(ctx, SERVICE_STARTING);
    return 0;
}

int service_check(service_ctx_t *ctx) {
    if (ctx->target_pid > 0 && process_exists(ctx->target_pid)) return 1;
    pid_t found = service_find_process(PROXY_BIN_NAME);
    if (found > 0) {
        ctx->target_pid = found;
        return 1;
    }
    return 0;
}

int service_get_pid(service_ctx_t *ctx) {
    if (ctx->target_pid > 0 && process_exists(ctx->target_pid)) return ctx->target_pid;
    ctx->target_pid = service_find_process(PROXY_BIN_NAME);
    return ctx->target_pid;
}

int service_monitor(service_ctx_t *ctx) {
    if (!ctx) return -1;
    if (ctx->state == SERVICE_RUNNING) {
        if (!service_check(ctx)) {
            LOG_WARN("Service died unexpectedly");
            service_transition(ctx, SERVICE_FAILED);
            if (!service_cooldown_active(ctx)) {
                service_restart_async(ctx);
                ctx->last_restart_time = time(NULL);
            }
        }
    }
    service_rotate_log(ctx);
    return 0;
}

int service_get_fd(service_ctx_t *ctx) { (void)ctx; return -1; }
void service_handle(service_ctx_t *ctx) { if (ctx) service_monitor(ctx); }
