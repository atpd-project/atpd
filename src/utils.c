#include "utils.h"
#include "logger.h"
#include "atp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/times.h>
#include <time.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <pthread.h>

#define CPU_CACHE_MAX 64

struct cpu_cache_entry {
    pid_t pid;
    double last_total;
    time_t last_time;
};

static struct cpu_cache_entry g_cpu_cache[CPU_CACHE_MAX];
static int g_cpu_cache_count = 0;
static pthread_mutex_t g_cpu_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

int mkdir_recursive(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;

    if (!path || !*path) return -1;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    if (len == 0) return -1;

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) == -1 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) == -1 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

int exec_cmd(const char *cmd, char *output, size_t output_size, int timeout_sec) {
    if (!cmd) return -1;

    LOG_EXEC(cmd);

    char timeout_cmd[MAX_CMD_LEN];
    int ret = snprintf(timeout_cmd, sizeof(timeout_cmd), "timeout %d %s", timeout_sec, cmd);
    if (ret < 0 || ret >= (int)sizeof(timeout_cmd)) {
        LOG_ERROR("command too long");
        return -1;
    }

    FILE *fp = popen(timeout_cmd, "r");
    if (!fp) {
        LOG_ERROR("popen failed: %s", strerror(errno));
        return -1;
    }

    if (output && output_size > 0) {
        if (output_size < 2) {
            output[0] = '\0';
        } else {
            size_t len = fread(output, 1, output_size - 1, fp);
            if (ferror(fp)) {
                LOG_ERROR("fread failed");
                pclose(fp);
                return -1;
            }
            output[len] = '\0';
            trim(output);
        }
    }

    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (exit_code == 124) {
        LOG_ERROR("Command timed out after %d seconds: %s", timeout_sec, cmd);
    }

    return exit_code;
}

int exec_cmd_simple(const char *cmd, int timeout_sec) {
    return exec_cmd(cmd, NULL, 0, timeout_sec);
}

int exec_cmd_argv(const char *cmd_path, char *const argv[], char *output, size_t output_size, int timeout_sec) {
    if (!cmd_path || !argv || !argv[0]) {
        LOG_ERROR("exec_cmd_argv: invalid arguments");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        LOG_ERROR("pipe failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execvp(cmd_path, argv);
        _exit(127);
    }

    close(pipefd[1]);

    if (output && output_size > 0) {
        size_t total = 0;

        while (total < output_size - 1) {
            ssize_t n = read(pipefd[0], output + total, output_size - 1 - total);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG_ERROR("read failed: %s", strerror(errno));
                output[0] = '\0';
                break;
            }

            if (n == 0) {
                break;
            }

            total += n;
        }

        if (total >= output_size - 1) {
            LOG_WARN("output truncated (%zu bytes)", output_size);
        }

        output[total] = '\0';
        trim(output);
    }

    close(pipefd[0]);

    int status;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return -1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) / 1000000000.0;

        if (elapsed >= timeout_sec) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            LOG_ERROR("command timeout: %s", cmd_path);
            return 124;
        }

        usleep(100000);
    }
}

int read_file(const char *path, char *buf, size_t buf_size) {
    if (!buf || buf_size < 2) return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    size_t len = fread(buf, 1, buf_size - 1, fp);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    buf[len] = '\0';

    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }

    fclose(fp);
    return (int)len;
}

int write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    if (fprintf(fp, "%s", content) < 0) {
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) {
        return -1;
    }

    return 0;
}

int is_number(const char *str) {
    if (!str || !*str) return 0;
    while (*str) {
        if (!isdigit((unsigned char)*str)) return 0;
        str++;
    }
    return 1;
}

void trim(char *str) {
    char *start = str;
    char *end;

    if (!str || !*str) return;

    while (isspace((unsigned char)*start)) start++;

    if (*start == '\0') {
        str[0] = '\0';
        return;
    }

    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;

    end[1] = '\0';

    if (start != str) {
        memmove(str, start, end - start + 2);
    }
}

int starts_with(const char *str, const char *prefix) {
    if (!str || !prefix) return 0;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

int ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > str_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

char *str_replace(const char *str, const char *old, const char *new_str) {
    if (!str || !old || !new_str) return NULL;

    size_t old_len = strlen(old);
    if (old_len == 0) {
        return strdup(str);
    }

    size_t new_len = strlen(new_str);
    size_t str_len = strlen(str);

    size_t count = 0;
    const char *p = str;
    while ((p = strstr(p, old)) != NULL) {
        count++;
        p += old_len;
    }

    size_t buf_len = str_len + count * (new_len - old_len) + 1;
    char *buf = malloc(buf_len);
    if (!buf) {
        LOG_ERROR("str_replace: malloc failed");
        return NULL;
    }

    char *dst = buf;
    const char *src = str;
    while ((p = strstr(src, old)) != NULL) {
        size_t len = p - src;
        memcpy(dst, src, len);
        dst += len;
        memcpy(dst, new_str, new_len);
        dst += new_len;
        src = p + old_len;
    }
    strcpy(dst, src);

    return buf;
}

int find_command_path(const char *name, char *out_path, size_t out_size) {
    if (!name || !*name || !out_path || out_size == 0) return -1;

    const char *search_paths[] = {
        "/data/adb/atp/bin",
        "/system/bin",
        "/system/xbin",
        "/vendor/bin",
        NULL
    };

    for (int i = 0; search_paths[i] != NULL; i++) {
        snprintf(out_path, out_size, "%s/%s", search_paths[i], name);
        if (access(out_path, X_OK) == 0) {
            return 0;
        }
    }

    return -1;
}

int get_pid_by_name(const char *name) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;

    struct dirent *entry;
    int found_pid = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;

        char *endptr;
        long pid_long = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid_long <= 0 || pid_long > INT_MAX) {
            continue;
        }
        pid_t pid = (pid_t)pid_long;

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

int process_exists(pid_t pid) {
    return kill(pid, 0) == 0;
}

long get_process_memory_kb(pid_t pid) {
    char path[PATH_MAX];
    char line[256];
    long vmrss = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line, "VmRSS: %ld", &vmrss);
            break;
        }
    }

    fclose(fp);
    return vmrss;
}

int get_process_threads(pid_t pid) {
    char path[PATH_MAX];
    char line[256];
    int threads = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Threads:", 8) == 0) {
            sscanf(line, "Threads: %d", &threads);
            break;
        }
    }

    fclose(fp);
    return threads;
}

int get_process_fd_count(pid_t pid) {
    char path[PATH_MAX];
    int count = 0;

    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR *dir = opendir(path);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') count++;
    }

    closedir(dir);
    return count;
}

double get_process_cpu_percent(pid_t pid) {
    char path[PATH_MAX];
    char line[256];
    unsigned long utime = 0, stime = 0;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);

    if (ticks_per_sec <= 0) return 0.0;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0.0;

    if (fgets(line, sizeof(line), fp)) {
        sscanf(line, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &utime, &stime);
    }
    fclose(fp);

    double total_time = (double)(utime + stime) / ticks_per_sec;

    pthread_mutex_lock(&g_cpu_cache_mutex);

    double cpu_percent = 0.0;
    int found = -1;
    time_t now = time(NULL);

    for (int i = 0; i < g_cpu_cache_count; i++) {
        if (g_cpu_cache[i].pid == pid) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        struct cpu_cache_entry *entry = &g_cpu_cache[found];
        if (entry->last_time > 0 && now > entry->last_time) {
            double elapsed = now - entry->last_time;
            if (elapsed > 0) {
                cpu_percent = ((total_time - entry->last_total) / elapsed) * 100.0;
                if (cpu_percent < 0) cpu_percent = 0;
                if (cpu_percent > 100) cpu_percent = 100;
            }
        }
        entry->last_total = total_time;
        entry->last_time = now;
    } else if (g_cpu_cache_count < CPU_CACHE_MAX) {
        g_cpu_cache[g_cpu_cache_count].pid = pid;
        g_cpu_cache[g_cpu_cache_count].last_total = total_time;
        g_cpu_cache[g_cpu_cache_count].last_time = now;
        g_cpu_cache_count++;
    }

    pthread_mutex_unlock(&g_cpu_cache_mutex);

    return cpu_percent;
}

int get_process_uptime_sec(pid_t pid) {
    char path[PATH_MAX];
    char line[256];
    unsigned long long start_time = 0;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);

    if (ticks_per_sec <= 0) return 0;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    if (fgets(line, sizeof(line), fp)) {
        char *p = line;
        int field = 0;
        char *saveptr;
        char *token = strtok_r(p, " ", &saveptr);
        while (token && field < 21) {
            token = strtok_r(NULL, " ", &saveptr);
            field++;
        }
        if (token && field == 21) {
            start_time = strtoull(token, NULL, 10);
        }
    }
    fclose(fp);

    if (start_time == 0) return 0;

    FILE *fp_uptime = fopen("/proc/uptime", "r");
    if (!fp_uptime) return 0;

    double uptime_sec = 0;
    if (fscanf(fp_uptime, "%lf", &uptime_sec) != 1) {
        fclose(fp_uptime);
        return 0;
    }
    fclose(fp_uptime);

    double process_start = (double)start_time / ticks_per_sec;
    int elapsed = (int)(uptime_sec - process_start);
    if (elapsed < 0) elapsed = 0;

    return elapsed;
}

int get_process_user_group(pid_t pid, char *user, char *group, size_t size) {
    if (!user || !group || size == 0) return -1;

    char path[PATH_MAX];
    char line[256];
    uid_t uid = 0;
    gid_t gid = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            sscanf(line, "Uid: %u", &uid);
        } else if (strncmp(line, "Gid:", 4) == 0) {
            sscanf(line, "Gid: %u", &gid);
        }
    }
    fclose(fp);

    if (snprintf(user, size, "%u", uid) < 0) return -1;
    if (snprintf(group, size, "%u", gid) < 0) return -1;
    return 0;
}

int get_binary_version(const char *bin_path, char *version, size_t size) {
    char cmd[MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "%s version 2>/dev/null | head -1", bin_path);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        snprintf(version, size, "%s", "unknown");
        return -1;
    }

    char output[256];
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        char *p = strrchr(output, ' ');
        if (p) {
            snprintf(version, size, "%s", p + 1);
        } else {
            snprintf(version, size, "%s", output);
        }
        return 0;
    }

    snprintf(version, size, "%s", "unknown");
    return -1;
}

void format_uptime(int seconds, char *buf, size_t size) {
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;

    if (hours > 0) {
        snprintf(buf, size, "%dh%02dm%02ds", hours, mins, secs);
    } else if (mins > 0) {
        snprintf(buf, size, "%dm%02ds", mins, secs);
    } else {
        snprintf(buf, size, "%ds", secs);
    }
}

int kill_process(pid_t pid, int signal) {
    return kill(pid, signal);
}

int kill_all_by_name(const char *name, int signal) {
    int killed = 0;
    DIR *dir = opendir("/proc");
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;

        char *endptr;
        long pid_long = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid_long <= 0 || pid_long > INT_MAX) {
            continue;
        }
        pid_t pid = (pid_t)pid_long;

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
    return killed;
}

int wait_for_pid_exit(pid_t pid, int timeout_sec) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        if (kill(pid, 0) != 0) {
            return 0;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) / 1000000000.0;

        if (elapsed >= timeout_sec) {
            break;
        }

        usleep(200000);
    }

    return -1;
}
