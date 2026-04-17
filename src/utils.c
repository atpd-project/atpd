#include "utils.h"
#include "logger.h"
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

int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

int mkdir_recursive(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    if (len == 0) return -1;
    
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    
    return mkdir(tmp, mode);
}

int exec_cmd(const char *cmd, char *output, size_t output_size, int timeout_sec) {
    LOG_EXEC(cmd);
    
    char timeout_cmd[MAX_CMD_LEN];
    snprintf(timeout_cmd, sizeof(timeout_cmd), "timeout %d %s", timeout_sec, cmd);
    
    FILE *fp = popen(timeout_cmd, "r");
    if (!fp) {
        LOG_ERROR("popen failed: %s", strerror(errno));
        return -1;
    }
    
    if (output && output_size > 0) {
        size_t len = fread(output, 1, output_size - 1, fp);
        output[len] = '\0';
        trim(output);
    }
    
    int ret = pclose(fp);
    int status = WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;
    
    if (status == 124) {
        LOG_ERROR("Command timed out after %d seconds: %s", timeout_sec, cmd);
    }
    
    return status;
}

int exec_cmd_simple(const char *cmd, int timeout_sec) {
    return exec_cmd(cmd, NULL, 0, timeout_sec);
}

int exec_cmd_argv(const char *cmd_path, char *const argv[], char *output, size_t output_size, int timeout_sec) {
    char cmd_buf[MAX_CMD_LEN];
    cmd_buf[0] = '\0';
    
    strncat(cmd_buf, cmd_path, sizeof(cmd_buf) - 1);
    for (int i = 1; argv[i] != NULL; i++) {
        strncat(cmd_buf, " ", sizeof(cmd_buf) - strlen(cmd_buf) - 1);
        strncat(cmd_buf, argv[i], sizeof(cmd_buf) - strlen(cmd_buf) - 1);
    }
    
    return exec_cmd(cmd_buf, output, output_size, timeout_sec);
}

int read_file(const char *path, char *buf, size_t buf_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    
    size_t len = fread(buf, 1, buf_size - 1, fp);
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
    
    fprintf(fp, "%s", content);
    fclose(fp);
    return 0;
}

int is_number(const char *str) {
    if (!str || !*str) return 0;
    while (*str) {
        if (!isdigit(*str)) return 0;
        str++;
    }
    return 1;
}

void trim(char *str) {
    char *start = str;
    char *end;
    
    if (!str || !*str) return;
    
    while (isspace(*start)) start++;
    
    if (*start == 0) {
        str[0] = '\0';
        return;
    }
    
    end = start + strlen(start) - 1;
    while (end > start && isspace(*end)) end--;
    
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
    static char buffer[4096];
    char *pos = buffer;
    const char *src = str;
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_str);
    
    if (!str || !old || !new_str) return (char*)str;
    
    while (*src) {
        if (strncmp(src, old, old_len) == 0) {
            strcpy(pos, new_str);
            pos += new_len;
            src += old_len;
        } else {
            *pos++ = *src++;
        }
    }
    *pos = '\0';
    return buffer;
}

int find_command_path(const char *name, char *out_path, size_t out_size) {
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
    
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0.0;
    
    if (fgets(line, sizeof(line), fp)) {
        sscanf(line, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &utime, &stime);
    }
    fclose(fp);
    
    double total_time = (double)(utime + stime) / ticks_per_sec;
    
    static double last_total = 0;
    static time_t last_time = 0;
    double cpu_percent = 0.0;
    
    time_t now = time(NULL);
    if (last_time > 0 && now > last_time) {
        double elapsed = now - last_time;
        cpu_percent = ((total_time - last_total) / elapsed) * 100.0;
        if (cpu_percent < 0) cpu_percent = 0;
        if (cpu_percent > 100) cpu_percent = 100;
    }
    
    last_total = total_time;
    last_time = now;
    
    return cpu_percent;
}

int get_process_uptime_sec(pid_t pid) {
    char path[PATH_MAX];
    char line[256];
    unsigned long start_time = 0;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    
    if (fgets(line, sizeof(line), fp)) {
        sscanf(line, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %lu",
               &start_time);
    }
    fclose(fp);
    
    if (start_time == 0) return 0;
    
    FILE *fp_uptime = fopen("/proc/uptime", "r");
    if (!fp_uptime) return 0;
    
    double uptime_sec;
    fscanf(fp_uptime, "%lf", &uptime_sec);
    fclose(fp_uptime);
    
    int elapsed = (int)uptime_sec - (int)(start_time / ticks_per_sec);
    if (elapsed < 0) elapsed = 0;
    
    return elapsed;
}

int get_process_user_group(pid_t pid, char *user, char *group, size_t size) {
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
    
    snprintf(user, size, "%u", uid);
    snprintf(group, size, "%u", gid);
    return 0;
}

int get_binary_version(const char *bin_path, char *version, size_t size) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "%s version 2>/dev/null | head -1", bin_path);
    
    char output[256];
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        char *p = strrchr(output, ' ');
        if (p) {
            strncpy(version, p + 1, size - 1);
        } else {
            strncpy(version, output, size - 1);
        }
        version[size - 1] = '\0';
        return 0;
    }
    
    strncpy(version, "unknown", size - 1);
    return -1;
}

void format_uptime(int seconds, char *buf, size_t size) {
    int days = seconds / 86400;
    int hours = (seconds % 86400) / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    
    if (days > 0) {
        snprintf(buf, size, "%dd %02d:%02d:%02d", days, hours, mins, secs);
    } else {
        snprintf(buf, size, "%02d:%02d:%02d", hours, mins, secs);
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
    return killed;
}

int wait_for_pid_exit(pid_t pid, int timeout_sec) {
    int waited = 0;
    while (waited < timeout_sec) {
        if (kill(pid, 0) != 0) {
            return 0;
        }
        sleep(1);
        waited++;
    }
    return -1;
}
