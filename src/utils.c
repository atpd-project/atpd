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
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#define CPU_CACHE_MAX 64

struct cpu_cache_entry {
    pid_t pid;
    unsigned long long starttime_ticks;
    double last_total;
    uint64_t last_time_ms;
};

static struct cpu_cache_entry g_cpu_cache[CPU_CACHE_MAX];
static int g_cpu_cache_count = 0;
static pthread_mutex_t g_cpu_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static int read_proc_stat(pid_t pid, unsigned long long *utime_out,
                          unsigned long long *stime_out,
                          unsigned long long *starttime_out);

int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

int mkdir_recursive(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;

    if (!path || !*path) return -1;

    int written = snprintf(tmp, sizeof(tmp), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    len = strlen(tmp);

    if (len == 0) return -1;

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            int mkdir_result = mkdir(tmp, mode);
            if (mkdir_result < 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            if (mkdir_result < 0 && errno == EEXIST) {
                struct stat st;
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    *p = '/';
                    errno = ENOTDIR;
                    return -1;
                }
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) == -1) {
        if (errno != EEXIST) return -1;
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
    }

    return 0;
}

int exec_cmd_argv(const char *cmd_path, char *const argv[], char *output, size_t output_size, int timeout_sec) {
    if (!cmd_path || !argv || !argv[0]) {
        LOG_ERROR("exec_cmd_argv: invalid arguments");
        return -1;
    }

    if (timeout_sec < 0) timeout_sec = 0;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) {
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
        int flags = fcntl(pipefd[1], F_GETFL, 0);
        if (flags >= 0) fcntl(pipefd[1], F_SETFL, flags & ~O_NONBLOCK);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);

        execvp(cmd_path, argv);
        _exit(127);
    }

    close(pipefd[1]);
    if (output && output_size > 0) output[0] = '\0';
    size_t total = 0;
    int status = 0, child_done = 0, pipe_done = 0, truncated = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t deadline = (uint64_t)start.tv_sec * 1000 + (uint64_t)start.tv_nsec / 1000000 +
                        (uint64_t)timeout_sec * 1000;
    while (!child_done || !pipe_done) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) child_done = 1;
        else if (result < 0 && errno != EINTR) child_done = 1;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
        if (now_ms >= deadline) {
            if (!child_done) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
            close(pipefd[0]);
            if (output && output_size > 0) output[total] = '\0';
            LOG_ERROR("command timeout: %s", cmd_path);
            return 124;
        }

        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int wait_ms = (int)((deadline - now_ms) > 100 ? 100 : (deadline - now_ms));
        int ready;
        do { ready = poll(&pfd, 1, wait_ms); } while (ready < 0 && errno == EINTR);
        if (ready < 0) break;
        if (ready == 0) continue;
        for (;;) {
            char discard[256];
            char *dst = output && output_size > 0 && total < output_size - 1 ? output + total : discard;
            size_t cap = dst == discard ? sizeof(discard) : output_size - 1 - total;
            ssize_t n = read(pipefd[0], dst, cap);
            if (n > 0) {
                if (dst != discard) total += (size_t)n;
                else truncated = 1;
                continue;
            }
            if (n == 0) { pipe_done = 1; close(pipefd[0]); break; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            pipe_done = 1;
            close(pipefd[0]);
            break;
        }
    }
    if (output && output_size > 0) { output[total] = '\0'; trim(output); }
    if (truncated) LOG_WARN("output truncated (%zu bytes)", output_size);
    if (!child_done || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
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

    size_t buf_len;
    if (new_len >= old_len) {
        size_t delta = new_len - old_len;
        if (delta != 0 && count > (SIZE_MAX - str_len - 1) / delta) return NULL;
        buf_len = str_len + count * delta + 1;
    } else {
        size_t delta = old_len - new_len;
        if (count > str_len / delta) return NULL;
        buf_len = str_len - count * delta + 1;
    }
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

    /* 1. Check in dynamic app_dir/bin and app_dir */
    char app_dir[PATH_MAX];
    if (get_app_dir(app_dir, sizeof(app_dir)) == 0) {
        snprintf(out_path, out_size, "%s/bin/%s", app_dir, name);
        if (access(out_path, X_OK) == 0) return 0;

        snprintf(out_path, out_size, "%s/%s", app_dir, name);
        if (access(out_path, X_OK) == 0) return 0;
    }

#ifdef ATP_DEFAULT_DIR
    snprintf(out_path, out_size, "%s/bin/%s", ATP_DEFAULT_DIR, name);
    if (access(out_path, X_OK) == 0) return 0;
    snprintf(out_path, out_size, "%s/%s", ATP_DEFAULT_DIR, name);
    if (access(out_path, X_OK) == 0) return 0;
#endif

    /* 2. Check in system PATH environment */
    const char *path_env = getenv("PATH");
    if (path_env && *path_env) {
        char path_copy[PATH_MAX * 2];
        strncpy(path_copy, path_env, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        char *saveptr = NULL;
        char *token = strtok_r(path_copy, ":", &saveptr);
        while (token) {
            if (*token) {
                snprintf(out_path, out_size, "%s/%s", token, name);
                if (access(out_path, X_OK) == 0) return 0;
            }
            token = strtok_r(NULL, ":", &saveptr);
        }
    }

    /* 3. Fallback standard system binary locations */
    const char *sys_paths[] = {
        "/system/bin", "/system/xbin", "/vendor/bin",
        "/usr/local/bin", "/usr/bin", "/bin", NULL
    };
    for (int i = 0; sys_paths[i] != NULL; i++) {
        snprintf(out_path, out_size, "%s/%s", sys_paths[i], name);
        if (access(out_path, X_OK) == 0) return 0;
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

        /* 1. Check comm */
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

        /* 2. Check exe link */
        snprintf(path, sizeof(path), "/proc/%d/exe", pid);
        ssize_t len = readlink(path, line, sizeof(line) - 1);
        if (len > 0) {
            line[len] = '\0';
            char exe_copy[PATH_MAX];
            strncpy(exe_copy, line, sizeof(exe_copy) - 1);
            exe_copy[sizeof(exe_copy) - 1] = '\0';
            char *base = basename(exe_copy);
            if (base && strcmp(base, name) == 0) {
                found_pid = pid;
                break;
            }
        }

    }

    closedir(dir);
    return found_pid;
}

int process_exists(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

int get_process_starttime(pid_t pid, unsigned long long *out_ticks) {
    if (!out_ticks) return -1;
    return read_proc_stat(pid, NULL, NULL, out_ticks);
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

int get_process_socket_count(pid_t pid) {
    char path[PATH_MAX];
    int count = 0;

    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR *dir = opendir(path);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char link_path[PATH_MAX];
        char target[256];
        snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%s", pid, entry->d_name);
        ssize_t len = readlink(link_path, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (strncmp(target, "socket:[", 8) == 0) {
                count++;
            }
        }
    }

    closedir(dir);
    return count;
}

static int read_proc_stat(pid_t pid, unsigned long long *utime_out,
                          unsigned long long *stime_out,
                          unsigned long long *starttime_out) {
    char path[PATH_MAX];
    char line[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp || !fgets(line, sizeof(line), fp)) {
        if (fp) fclose(fp);
        return -1;
    }
    fclose(fp);
    char *close_comm = strrchr(line, ')');
    if (!close_comm) return -1;
    char *cursor = close_comm + 1;
    while (*cursor == ' ') cursor++;
    /* /proc/<pid>/stat field 3 (state) is a single character. */
    if (!*cursor || isspace((unsigned char)*cursor)) return -1;
    cursor++;
    if (*cursor && !isspace((unsigned char)*cursor)) return -1;
    char *end;
    unsigned long long utime = 0, stime = 0, starttime = 0;
    for (int field = 4; field <= 22; field++) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) return -1;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return -1;
        if (field == 14) utime = value;
        else if (field == 15) stime = value;
        else if (field == 22) starttime = value;
        cursor = end;
    }
    if (utime_out) *utime_out = utime;
    if (stime_out) *stime_out = stime;
    if (starttime_out) *starttime_out = starttime;
    return 0;
}

double get_process_cpu_percent(pid_t pid) {
    unsigned long long utime = 0, stime = 0, starttime = 0;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);

    if (ticks_per_sec <= 0) return 0.0;
    if (read_proc_stat(pid, &utime, &stime, &starttime) != 0) return 0.0;

    double total_time = (double)(utime + stime) / ticks_per_sec;

    pthread_mutex_lock(&g_cpu_cache_mutex);

    double cpu_percent = 0.0;
    int found = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    for (int i = 0; i < g_cpu_cache_count; i++) {
        if (g_cpu_cache[i].pid == pid) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        struct cpu_cache_entry *entry = &g_cpu_cache[found];
        if (entry->starttime_ticks != starttime) {
            entry->starttime_ticks = starttime;
            entry->last_total = total_time;
            entry->last_time_ms = now;
            pthread_mutex_unlock(&g_cpu_cache_mutex);
            return 0.0;
        }
        if (entry->last_time_ms > 0 && now > entry->last_time_ms) {
            double elapsed = (double)(now - entry->last_time_ms) / 1000.0;
            if (elapsed > 0) {
                cpu_percent = ((total_time - entry->last_total) / elapsed) * 100.0;
                if (cpu_percent < 0) cpu_percent = 0;
                if (cpu_percent > 100) cpu_percent = 100;
            }
        }
        entry->last_total = total_time;
        entry->last_time_ms = now;
    } else if (g_cpu_cache_count < CPU_CACHE_MAX) {
        g_cpu_cache[g_cpu_cache_count].pid = pid;
        g_cpu_cache[g_cpu_cache_count].starttime_ticks = starttime;
        g_cpu_cache[g_cpu_cache_count].last_total = total_time;
        g_cpu_cache[g_cpu_cache_count].last_time_ms = now;
        g_cpu_cache_count++;
    }

    pthread_mutex_unlock(&g_cpu_cache_mutex);

    return cpu_percent;
}

int get_process_uptime_sec(pid_t pid) {
    unsigned long long start_time = 0;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);

    if (ticks_per_sec <= 0) return 0;

    if (read_proc_stat(pid, NULL, NULL, &start_time) != 0 || start_time == 0) return 0;

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
    int have_uid = 0;
    int have_gid = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            have_uid = sscanf(line, "Uid: %u", &uid) == 1;
        } else if (strncmp(line, "Gid:", 4) == 0) {
            have_gid = sscanf(line, "Gid: %u", &gid) == 1;
        }
    }
    fclose(fp);

    if (!have_uid || !have_gid) return -1;
    if (snprintf(user, size, "%u", uid) < 0 || snprintf(group, size, "%u", gid) < 0) return -1;
    return 0;
}

int get_binary_version(const char *bin_path, char *version, size_t size) {
    if (!bin_path || !bin_path[0] || !version || size == 0) {
        if (version && size > 0) snprintf(version, size, "%s", "unknown");
        return -1;
    }

    static char s_cached_path[PATH_MAX] = {0};
    static char s_cached_ver[64] = {0};

    if (s_cached_path[0] && strcmp(s_cached_path, bin_path) == 0 && s_cached_ver[0]) {
        snprintf(version, size, "%s", s_cached_ver);
        return 0;
    }

    char output[256] = {0};
    char *argv[] = { (char *)bin_path, "version", NULL };
    if (exec_cmd_argv(bin_path, argv, output, sizeof(output), 3) == 0 && output[0]) {
        trim(output);
        char *p = strstr(output, "version ");
        if (p) {
            p += 8;
            char *space = strchr(p, ' ');
            if (space) *space = '\0';
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            snprintf(version, size, "%s", p);
            snprintf(s_cached_path, sizeof(s_cached_path), "%s", bin_path);
            snprintf(s_cached_ver, sizeof(s_cached_ver), "%s", p);
            return 0;
        }
        char *last_space = strrchr(output, ' ');
        const char *v = last_space ? last_space + 1 : output;
        snprintf(version, size, "%s", v);
        snprintf(s_cached_path, sizeof(s_cached_path), "%s", bin_path);
        strncpy(s_cached_ver, v, sizeof(s_cached_ver) - 1);
        s_cached_ver[sizeof(s_cached_ver) - 1] = '\0';
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

int get_app_dir(char *buf, size_t size) {
    if (!buf || size == 0) return -1;

    char exe_path[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash && slash != exe_path) {
            *slash = '\0';
            /* If the binary resides in a 'bin' subdirectory, point to its parent module root */
            char *sub = strrchr(exe_path, '/');
            if (sub && strcmp(sub + 1, "bin") == 0 && sub != exe_path) {
                *sub = '\0';
            }
            snprintf(buf, size, "%s", exe_path);
            return 0;
        }
    }

    /* Fallback checks for standard configurable installation prefix */
#ifdef ATP_DEFAULT_DIR
    if (access(ATP_DEFAULT_DIR, F_OK) == 0) {
        snprintf(buf, size, "%s", ATP_DEFAULT_DIR);
        return 0;
    }
#endif

    snprintf(buf, size, ".");
    return 0;
}

#define TZ_NAME_MAX 64
#define TZ_PATH_MAX 256

/* Android tzdata binary structure definitions */
struct tzdata_hdr {
    char magic_version[12]; /* "tzdata2024a\0" */
    uint32_t index_offset;   /* Big-endian */
    uint32_t data_offset;    /* Big-endian */
    uint32_t zonetab_offset; /* Big-endian */
};

struct tzdata_idx_entry {
    char name[40];           /* Null-terminated name, e.g. "Asia/Shanghai" */
    uint32_t start_offset;   /* Big-endian relative to data_offset */
    uint32_t length;         /* Big-endian byte count of TZif payload */
    uint32_t unused;
};

/* Standard Android tzdata file candidates in priority order */
static const char *k_android_tzdata_paths[] = {
    "/apex/com.android.tzdata/etc/tz/tzdata",
    "/apex/com.android.runtime/etc/tz/tzdata",
    "/system/usr/share/zoneinfo/tzdata",
    "/data/misc/zoneinfo/current/tzdata",
    "/system/etc/tzdata",
    "/vendor/etc/tzdata",
    NULL
};

/* Common timezone fallback table for POSIX TZ strings */
struct tz_posix_entry {
    const char *name;
    const char *posix_tz;
};

static const struct tz_posix_entry k_tz_fallbacks[] = {
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Chongqing", "CST-8"},
    {"Asia/Harbin", "CST-8"},
    {"Asia/Urumqi", "XJT-6"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Macau", "CST-8"},
    {"Asia/Taipei", "CST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Calcutta", "IST-5:30"},
    {"Asia/Bangkok", "ICT-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Dubai", "GST-4"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Moscow", "MSK-3"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"UTC", "UTC0"},
    {"GMT", "GMT0"},
    {NULL, NULL}
};

static pthread_mutex_t g_tz_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_tz_name[TZ_NAME_MAX] = {0};
static char g_tz_localtime_file[TZ_PATH_MAX] = {0};
static int g_tz_initialized = 0;

static const char *lookup_posix_fallback(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; k_tz_fallbacks[i].name != NULL; i++) {
        if (strcmp(k_tz_fallbacks[i].name, name) == 0) {
            return k_tz_fallbacks[i].posix_tz;
        }
    }
    return NULL;
}

/* Extract POSIX TZ string from the footer of a TZif v2/v3 file if present */
static int extract_posix_tz_from_tzif(const uint8_t *buf, size_t size, char *out_tz, size_t out_size) {
    if (!buf || size < 8 || !out_tz || out_size == 0) return -1;
    if (memcmp(buf, "TZif2", 5) != 0 && memcmp(buf, "TZif3", 5) != 0) {
        return -1;
    }

    if (buf[size - 1] != '\n') return -1;
    ssize_t idx = (ssize_t)size - 2;
    while (idx >= 0 && buf[idx] != '\n') {
        idx--;
    }
    if (idx < 0) return -1;

    size_t tz_len = (size - 1) - (size_t)(idx + 1);
    if (tz_len == 0 || tz_len >= out_size) return -1;

    memcpy(out_tz, buf + idx + 1, tz_len);
    out_tz[tz_len] = '\0';
    return 0;
}

/* Read Android system property */
static int get_android_prop(const char *key, char *out_val, size_t size) {
    if (!key || !out_val || size == 0) return -1;
    out_val[0] = '\0';

#if defined(__ANDROID__)
    char prop_val[92] = {0};
    int len = __system_property_get(key, prop_val);
    if (len > 0 && prop_val[0]) {
        strncpy(out_val, prop_val, size - 1);
        out_val[size - 1] = '\0';
        trim(out_val);
        if (out_val[0]) return 0;
    }
#endif

    /* Universal fallback: execute /system/bin/getprop or getprop */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "/system/bin/getprop %s", key);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(cmd, sizeof(cmd), "getprop %s", key);
        fp = popen(cmd, "r");
    }
    if (fp) {
        if (fgets(out_val, (int)size, fp)) {
            trim(out_val);
        }
        pclose(fp);
        if (out_val[0]) return 0;
    }

    return -1;
}

/* Detect device timezone name from Android system properties or environment */
static int detect_device_timezone(char *out_name, size_t size) {
    if (!out_name || size == 0) return -1;
    out_name[0] = '\0';

    /* 1. Check Android system property: persist.sys.timezone */
    if (get_android_prop("persist.sys.timezone", out_name, size) == 0 && out_name[0]) {
        return 0;
    }

    /* 2. Fallback property: persist.sys.timezone.default */
    if (get_android_prop("persist.sys.timezone.default", out_name, size) == 0 && out_name[0]) {
        return 0;
    }

    /* 3. Fallback check: ro.product.locale */
    char locale[32] = {0};
    if (get_android_prop("ro.product.locale", locale, sizeof(locale)) == 0 && locale[0]) {
        if (strncasecmp(locale, "zh", 2) == 0) {
            strncpy(out_name, "Asia/Shanghai", size - 1);
            return 0;
        } else if (strncasecmp(locale, "ja", 2) == 0) {
            strncpy(out_name, "Asia/Tokyo", size - 1);
            return 0;
        } else if (strncasecmp(locale, "ko", 2) == 0) {
            strncpy(out_name, "Asia/Seoul", size - 1);
            return 0;
        }
    }

    return -1;
}

/* Extract TZif data for tz_name from an Android monolithic tzdata container */
static int extract_tzif_from_tzdata_file(const char *tzdata_path, const char *tz_name,
                                         const char *out_file, char *out_posix_tz, size_t out_posix_size) {
    if (!tzdata_path || !tz_name || !out_file) return -1;

    FILE *fp = fopen(tzdata_path, "rb");
    if (!fp) return -1;

    struct tzdata_hdr hdr;
    if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return -1;
    }

    if (strncmp(hdr.magic_version, "tzdata", 6) != 0) {
        fclose(fp);
        return -1;
    }

    uint32_t index_offset = ntohl(hdr.index_offset);
    uint32_t data_offset = ntohl(hdr.data_offset);

    if (data_offset <= index_offset || index_offset < sizeof(hdr)) {
        fclose(fp);
        return -1;
    }

    size_t num_entries = (data_offset - index_offset) / sizeof(struct tzdata_idx_entry);
    if (num_entries == 0 || num_entries > 4096) {
        fclose(fp);
        return -1;
    }

    if (fseek(fp, (long)index_offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    struct tzdata_idx_entry entry;
    int found = 0;

    for (size_t i = 0; i < num_entries; i++) {
        if (fread(&entry, 1, sizeof(entry), fp) != sizeof(entry)) {
            break;
        }
        entry.name[sizeof(entry.name) - 1] = '\0';
        if (strcmp(entry.name, tz_name) == 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        fclose(fp);
        return -1;
    }

    uint32_t start_offset = ntohl(entry.start_offset);
    uint32_t length = ntohl(entry.length);

    if (length < 4 || length > 1024 * 1024) {
        fclose(fp);
        return -1;
    }

    if (fseek(fp, (long)(data_offset + start_offset), SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(length);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    if (fread(buf, 1, length, fp) != length) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Validate TZif magic */
    if (memcmp(buf, "TZif", 4) != 0) {
        free(buf);
        return -1;
    }

    /* Optionally extract POSIX TZ string from footer */
    if (out_posix_tz && out_posix_size > 0) {
        extract_posix_tz_from_tzif(buf, length, out_posix_tz, out_posix_size);
    }

    /* Write TZif binary to output file */
    char tmp_file[TZ_PATH_MAX];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp.%d", out_file, getpid());

    FILE *out_fp = fopen(tmp_file, "wb");
    if (!out_fp) {
        free(buf);
        return -1;
    }

    size_t written = fwrite(buf, 1, length, out_fp);
    fclose(out_fp);
    free(buf);

    if (written != length) {
        unlink(tmp_file);
        return -1;
    }

    if (rename(tmp_file, out_file) != 0) {
        unlink(tmp_file);
        return -1;
    }

    return 0;
}

/* Locate Android tzdata file and extract timezone */
static int extract_android_tzdata(const char *tz_name, const char *out_file,
                                  char *out_posix_tz, size_t out_posix_size) {
    for (int i = 0; k_android_tzdata_paths[i] != NULL; i++) {
        const char *path = k_android_tzdata_paths[i];
        if (access(path, R_OK) == 0) {
            if (extract_tzif_from_tzdata_file(path, tz_name, out_file,
                                             out_posix_tz, out_posix_size) == 0) {
                return 0;
            }
        }
    }
    return -1;
}

/* Convert date +%z output (e.g. +0800, -0500) to POSIX TZ string */
static int derive_posix_tz_from_date(char *out_tz, size_t size) {
    if (!out_tz || size < 16) return -1;

    char out[32] = {0};
    FILE *fp = popen("/system/bin/date +%z", "r");
    if (!fp) {
        fp = popen("date +%z", "r");
    }
    if (!fp) return -1;

    if (fgets(out, sizeof(out), fp)) {
        trim(out);
    }
    pclose(fp);

    /* Format: +0800 or -0500 */
    if (strlen(out) != 5 || (out[0] != '+' && out[0] != '-')) {
        return -1;
    }

    char sign = out[0];
    int hours = (out[1] - '0') * 10 + (out[2] - '0');
    int mins = (out[3] - '0') * 10 + (out[4] - '0');

    /* POSIX TZ format: positive offset is West of UTC (-sign in POSIX) */
    char posix_sign = (sign == '+') ? '-' : '+';
    if (mins == 0) {
        snprintf(out_tz, size, "UTC%c%d", posix_sign, hours);
    } else {
        snprintf(out_tz, size, "UTC%c%d:%02d", posix_sign, hours, mins);
    }

    return 0;
}

/* Resolve appropriate run directory for localtime extraction */
static void resolve_localtime_target_path(char *out_path, size_t size) {
    char app_dir[PATH_MAX] = {0};
    get_app_dir(app_dir, sizeof(app_dir));

    char run_dir[PATH_MAX + 16];
    snprintf(run_dir, sizeof(run_dir), "%s/run", app_dir);
    if (mkdir_recursive(run_dir, 0755) == 0 && access(run_dir, W_OK) == 0) {
        snprintf(out_path, size, "%s/localtime", run_dir);
        return;
    }

    /* Fallback writable paths on Android */
    if (access("/data/local/tmp", W_OK) == 0) {
        snprintf(out_path, size, "/data/local/tmp/atpd_localtime");
        return;
    }

    snprintf(out_path, size, "/tmp/atpd_localtime");
}

int atp_timezone_init(void) {
    pthread_mutex_lock(&g_tz_mutex);

    /* 1. If TZ environment is explicitly set to a valid non-empty value, respect it */
    const char *env_tz = getenv("TZ");
    if (env_tz && env_tz[0]) {
        /* If TZ is a file path (starts with '/' or ':/') or valid POSIX TZ */
        if (env_tz[0] == '/' || env_tz[0] == ':' || strchr(env_tz, '-') || strchr(env_tz, '+') || strchr(env_tz, '0')) {
            tzset();
            strncpy(g_tz_name, env_tz, sizeof(g_tz_name) - 1);
            g_tz_initialized = 1;
            pthread_mutex_unlock(&g_tz_mutex);
            return 0;
        }
    }

    /* 2. Check if standard Linux /etc/localtime exists and is readable */
    if (access("/etc/localtime", R_OK) == 0) {
        tzset();
        if (!g_tz_name[0]) {
            strncpy(g_tz_name, "Localtime", sizeof(g_tz_name) - 1);
        }
        g_tz_initialized = 1;
        pthread_mutex_unlock(&g_tz_mutex);
        return 0;
    }

    /* 3. Detect Android Device Timezone */
    char detected_tz[TZ_NAME_MAX] = {0};
    if (detect_device_timezone(detected_tz, sizeof(detected_tz)) != 0 || !detected_tz[0]) {
        /* Fallback: try deriving from system date +%z */
        char date_posix_tz[32] = {0};
        if (derive_posix_tz_from_date(date_posix_tz, sizeof(date_posix_tz)) == 0) {
            setenv("TZ", date_posix_tz, 1);
            tzset();
            strncpy(g_tz_name, date_posix_tz, sizeof(g_tz_name) - 1);
            g_tz_initialized = 1;
            pthread_mutex_unlock(&g_tz_mutex);
            return 0;
        }
        /* Default to Asia/Shanghai for Chinese Android ecosystem if no detection */
        strncpy(detected_tz, "Asia/Shanghai", sizeof(detected_tz) - 1);
    }

    strncpy(g_tz_name, detected_tz, sizeof(g_tz_name) - 1);

    /* 4. Prepare target localtime file */
    if (!g_tz_localtime_file[0]) {
        resolve_localtime_target_path(g_tz_localtime_file, sizeof(g_tz_localtime_file));
    }

    char extracted_posix_tz[64] = {0};
    int extracted = extract_android_tzdata(detected_tz, g_tz_localtime_file,
                                          extracted_posix_tz, sizeof(extracted_posix_tz));

    if (extracted == 0 && access(g_tz_localtime_file, R_OK) == 0) {
        /* Both Musl libc and Glibc support TZ pointing to a file (with or without leading ':') */
        setenv("TZ", g_tz_localtime_file, 1);
        tzset();
        g_tz_initialized = 1;
        pthread_mutex_unlock(&g_tz_mutex);
        return 0;
    }

    /* 5. Fallback: use extracted POSIX string or fallback lookup table */
    const char *posix_tz = extracted_posix_tz[0] ? extracted_posix_tz : lookup_posix_fallback(detected_tz);
    if (posix_tz && posix_tz[0]) {
        setenv("TZ", posix_tz, 1);
        tzset();
        g_tz_initialized = 1;
        pthread_mutex_unlock(&g_tz_mutex);
        return 0;
    }

    /* 6. Ultimate fallback: derive from system date */
    char date_posix_tz[32] = {0};
    if (derive_posix_tz_from_date(date_posix_tz, sizeof(date_posix_tz)) == 0) {
        setenv("TZ", date_posix_tz, 1);
        tzset();
        g_tz_initialized = 1;
        pthread_mutex_unlock(&g_tz_mutex);
        return 0;
    }

    /* Default CST-8 */
    setenv("TZ", "CST-8", 1);
    tzset();
    g_tz_initialized = 1;
    pthread_mutex_unlock(&g_tz_mutex);
    return 0;
}

int atp_timezone_get_name(char *buf, size_t size) {
    if (!buf || size == 0) return -1;
    pthread_mutex_lock(&g_tz_mutex);
    if (!g_tz_initialized) {
        pthread_mutex_unlock(&g_tz_mutex);
        atp_timezone_init();
        pthread_mutex_lock(&g_tz_mutex);
    }
    strncpy(buf, g_tz_name[0] ? g_tz_name : "UTC", size - 1);
    buf[size - 1] = '\0';
    pthread_mutex_unlock(&g_tz_mutex);
    return 0;
}

long atp_timezone_get_offset_sec(void) {
    if (!g_tz_initialized) {
        atp_timezone_init();
    }
    time_t now = time(NULL);
    struct tm tm_local;
    if (!localtime_r(&now, &tm_local)) {
        return 0;
    }
#if defined(__ANDROID__) || defined(__USE_MISC) || defined(_GNU_SOURCE)
    return (long)tm_local.tm_gmtoff;
#else
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    tm_utc.tm_isdst = tm_local.tm_isdst;
    return (long)(mktime(&tm_local) - mktime(&tm_utc));
#endif
}
