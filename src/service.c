#include "service.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <signal.h>
#include <sys/wait.h>

int service_init(service_ctx_t *ctx, atp_config_t *cfg) {
    memset(ctx, 0, sizeof(service_ctx_t));
    
    snprintf(ctx->bin_path, sizeof(ctx->bin_path), "%s/bin/%s", cfg->data_dir, PROXY_BIN_NAME);
    snprintf(ctx->conf_path, sizeof(ctx->conf_path), "%s/sing-box/config.json", cfg->data_dir);
    snprintf(ctx->log_path, sizeof(ctx->log_path), "%s/run/sing-box.log", cfg->data_dir);
    snprintf(ctx->pid_path, sizeof(ctx->pid_path), "%s/run/sing-box.pid", cfg->data_dir);
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "%s/sing-box", cfg->data_dir);
    
    strncpy(ctx->user, cfg->core_user, sizeof(ctx->user) - 1);
    strncpy(ctx->group, cfg->core_group, sizeof(ctx->group) - 1);
    ctx->restart_cooldown_sec = 60;
    ctx->last_restart_time = 0;
    ctx->restart_failures = 0;
    ctx->state = SERVICE_STOPPED;
    
    LOG_DEBUG("Service initialized: bin=%s", ctx->bin_path);
    return 0;
}

int service_validate_config(service_ctx_t *ctx) {
    char cmd[MAX_CMD_LEN];
    char output[4096];
    
    snprintf(cmd, sizeof(cmd), "%s check -D \"%s\" 2>&1", ctx->bin_path, ctx->work_dir);
    
    int ret = exec_cmd(cmd, output, sizeof(output), 10);
    
    if (ret != 0) {
        LOG_ERROR("Config validation failed: %s", output);
        return -1;
    }
    
    LOG_DEBUG("Config validation passed");
    return 0;
}

static int set_user_group(service_ctx_t *ctx) {
    struct group *grp = getgrnam(ctx->group);
    if (grp) {
        if (setgid(grp->gr_gid) != 0) {
            LOG_ERROR("Failed to set group %s: %s", ctx->group, strerror(errno));
            return -1;
        }
    }
    
    struct passwd *pwd = getpwnam(ctx->user);
    if (pwd) {
        if (setuid(pwd->pw_uid) != 0) {
            LOG_ERROR("Failed to set user %s: %s", ctx->user, strerror(errno));
            return -1;
        }
    }
    
    return 0;
}

int service_start(service_ctx_t *ctx) {
    if (service_check(ctx)) {
        LOG_INFO("Service already running");
        return 0;
    }
    
    LOG_INFO("Starting service: %s", ctx->bin_path);
    ctx->state = SERVICE_STARTING;
    
    if (service_validate_config(ctx) != 0) {
        LOG_ERROR("Config validation failed, aborting start");
        ctx->state = SERVICE_FAILED;
        return -1;
    }
    
    service_kill_all(PROXY_BIN_NAME, SIGKILL);
    usleep(500000);
    
    char log_old[PATH_MAX];
    snprintf(log_old, sizeof(log_old), "%s.1", ctx->log_path);
    rename(ctx->log_path, log_old);
    
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("Fork failed: %s", strerror(errno));
        ctx->state = SERVICE_FAILED;
        return -1;
    }
    
    if (pid == 0) {
        setsid();
        
        signal(SIGPIPE, SIG_IGN);
        
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        FILE *fp = fopen(ctx->log_path, "a");
        if (fp) {
            dup2(fileno(fp), STDOUT_FILENO);
            dup2(fileno(fp), STDERR_FILENO);
            fclose(fp);
        } else {
            open("/dev/null", O_RDONLY);
            open("/dev/null", O_WRONLY);
            open("/dev/null", O_WRONLY);
        }
        
        set_user_group(ctx);
        
        char *argv[] = {
            (char *)ctx->bin_path,
            "run",
            "-D",
            (char *)ctx->work_dir,
            NULL
        };
        
        execv(ctx->bin_path, argv);
        exit(1);
    }
    
    FILE *fp = fopen(ctx->pid_path, "w");
    if (fp) {
        fprintf(fp, "%d\n", pid);
        fclose(fp);
    }
    
    ctx->state = SERVICE_RUNNING;
    LOG_INFO("Service started with PID %d", pid);
    
    return service_wait_ready(ctx, 10);
}

int service_stop(service_ctx_t *ctx) {
    LOG_INFO("Stopping service");
    ctx->state = SERVICE_STOPPING;
    
    int pid = service_get_pid(ctx);
    if (pid > 0) {
        kill(pid, SIGTERM);
        wait_for_pid_exit(pid, 5);
        kill(pid, SIGKILL);
    }
    
    service_kill_all(PROXY_BIN_NAME, SIGTERM);
    usleep(200000);
    service_kill_all(PROXY_BIN_NAME, SIGKILL);
    
    unlink(ctx->pid_path);
    
    ctx->state = SERVICE_STOPPED;
    LOG_INFO("Service stopped");
    return 0;
}

int service_restart(service_ctx_t *ctx) {
    LOG_INFO("Restarting service");
    service_stop(ctx);
    sleep(2);
    return service_start(ctx);
}

int service_check(service_ctx_t *ctx) {
    int pid = service_get_pid(ctx);
    if (pid > 0 && process_exists(pid)) {
        return 1;
    }
    
    char cmd[MAX_CMD_LEN];
    char output[256];
    
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", PROXY_BIN_NAME);
    if (exec_cmd(cmd, output, sizeof(output), 3) == 0 && output[0]) {
        return 1;
    }
    
    return 0;
}

int service_check_port(int port) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "netstat -tuln 2>/dev/null | grep -q \":%d \"", port);
    return exec_cmd_simple(cmd, 3) == 0;
}

int service_get_pid(service_ctx_t *ctx) {
    if (file_exists(ctx->pid_path)) {
        char pid_str[32];
        if (read_file(ctx->pid_path, pid_str, sizeof(pid_str)) > 0) {
            int pid = atoi(pid_str);
            if (pid > 0 && process_exists(pid)) {
                return pid;
            }
        }
    }
    
    char cmd[MAX_CMD_LEN];
    char output[256];
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null | awk '{print $1}'", PROXY_BIN_NAME);
    
    if (exec_cmd(cmd, output, sizeof(output), 3) == 0 && output[0]) {
        return atoi(output);
    }
    
    return -1;
}

int service_wait_ready(service_ctx_t *ctx, int timeout_sec) {
    int waited = 0;
    
    while (waited < timeout_sec) {
        if (service_check(ctx)) {
            LOG_INFO("Service is ready after %d seconds", waited);
            return 0;
        }
        sleep(1);
        waited++;
    }
    
    LOG_WARN("Service not ready after %d seconds", timeout_sec);
    return -1;
}

int service_rotate_log(service_ctx_t *ctx) {
    struct stat st;
    if (stat(ctx->log_path, &st) != 0) return 0;
    
    if (st.st_size < 10 * 1024 * 1024) return 0;
    
    char old_path[PATH_MAX];
    snprintf(old_path, sizeof(old_path), "%s.1", ctx->log_path);
    rename(ctx->log_path, old_path);
    
    LOG_INFO("Log rotated");
    return 0;
}

void service_set_cooldown(service_ctx_t *ctx, int seconds) {
    ctx->restart_cooldown_sec = seconds;
}

int service_cooldown_active(service_ctx_t *ctx) {
    time_t now = time(NULL);
    int elapsed = now - ctx->last_restart_time;
    
    if (elapsed < ctx->restart_cooldown_sec) {
        LOG_DEBUG("Cooldown active: %d seconds remaining", 
                  ctx->restart_cooldown_sec - elapsed);
        return 1;
    }
    
    return 0;
}

void service_reset_failures(service_ctx_t *ctx) {
    ctx->restart_failures = 0;
    ctx->last_restart_time = time(NULL);
}

pid_t service_find_process(const char *name) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    
    struct dirent *entry;
    pid_t found_pid = -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) continue;
        
        pid_t pid = atoi(entry->d_name);
        char path[PATH_MAX];
        char line[256];
        
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
    
    if (kill(pid, signal) != 0) {
        LOG_WARN("Failed to send signal %d to PID %d: %s", signal, pid, strerror(errno));
        return -1;
    }
    
    if (wait_sec > 0) {
        wait_for_pid_exit(pid, wait_sec);
    }
    
    return 0;
}

int service_kill_all(const char *name, int signal) {
    int killed = 0;
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) continue;
        
        pid_t pid = atoi(entry->d_name);
        char path[PATH_MAX];
        char line[256];
        
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
    if (killed > 0) {
        LOG_DEBUG("Killed %d processes with name %s", killed, name);
    }
    
    return killed;
}