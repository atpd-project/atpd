#include "atp.h"
#include "config.h"
#include "config_validator.h"
#include "logger.h"
#include "utils.h"
#include "service.h"
#include "api.h"
#include "netlink.h"
#include "app_filter.h"
#include "fcm_monitor.h"
#include "perf_mode.h"
#include "status.h"
#include "ui.h"
#include "cli.h"
#include "version.h"
#include "tproxy.h"
#include "routing.h"
#include "geoip.h"
#include "mac_filter.h"
#include "ipv6_manager.h"
#include "inet_diag.h"
#include "epoll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <libgen.h>
#include <signal.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)

api_ctx_t g_api_ctx;
atp_config_t g_config;
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_reload = 0;
volatile sig_atomic_t g_show_status = 0;

static void signal_handler(int sig) {
    if (sig == SIGHUP) g_reload = 1;
    else if (sig == SIGUSR1) g_show_status = 1;
    else g_running = 0;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    umask(0);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
    (void)chdir("/");
}

static int write_pid_file(const char *pid_file) {
    char dir[SAFE_PATH_MAX];
    if (!pid_file || strlen(pid_file) >= PATH_MAX) return -1;
    snprintf(dir, sizeof(dir), "%s", pid_file);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_recursive(dir, 0755); }
    FILE *fp = fopen(pid_file, "w");
    if (!fp) return -1;
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

static void get_binary_dir(char *buf, size_t size) {
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len != -1) {
        buf[len] = '\0';
        char *dir = dirname(buf);
        size_t dlen = strlen(dir);
        if (dlen > 0 && dlen < size) memmove(buf, dir, dlen + 1);
        else buf[0] = '\0';
    } else buf[0] = '\0';
}

static int find_config_file(char *path, size_t size, const char *user_path) {
    if (user_path && user_path[0]) {
        if (strlen(user_path) >= size) return -1;
        snprintf(path, size, "%s", user_path);
    } else {
        char bin_dir[PATH_MAX] = {0};
        get_binary_dir(bin_dir, sizeof(bin_dir));
        size_t dlen = strlen(bin_dir);
        if (dlen > 0 && dlen < (size - 16)) {
            memcpy(path, bin_dir, dlen);
            path[dlen] = '/';
            memcpy(path + dlen + 1, "atp.conf", 9);
        } else {
            snprintf(path, size, "./atp.conf");
        }
    }
    return access(path, R_OK);
}

static int confirm_operation(const char *op, int force) {
    if (force) return 1;
    char res[8];
    fprintf(stderr, "Warning: This will %s the ATP daemon. Are you sure? [y/N] ", op);
    if (!fgets(res, sizeof(res), stdin)) return 0;
    return (res[0] == 'y' || res[0] == 'Y');
}

static void run_event_loop(service_ctx_t *svc, api_ctx_t *api) {
    int nl_fd = netlink_get_fd();
    if (nl_fd >= 0) epoll_add_fd(nl_fd, netlink_handle_event, NULL);
    LOG_INFO("Event loop started");
    while (g_running) {
        epoll_run_once(100); fcm_monitor_poll();
        if (svc) service_monitor(svc);
        if (g_reload) { config_reload(&g_config); if (g_config.app_proxy_enable) app_filter_reload(&g_config); g_reload = 0; }
        if (g_show_status) { status_show(&g_config, svc, api); g_show_status = 0; }
    }
}

static int do_start(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX], pp[SAFE_PATH_MAX];
    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != 0) return 1;
    config_set_defaults(&g_config);
    snprintf(g_config.data_dir, sizeof(g_config.data_dir), "%s", ATP_DEFAULT_DIR);
    if (snprintf(pp, sizeof(pp), "%s/%s", g_config.data_dir, ATP_PID_FILE) >= (int)sizeof(pp)) return 1;
    if (file_exists(pp)) {
        FILE *f = fopen(pp, "r");
        if (f) {
            int pid;
            if (fscanf(f, "%d", &pid) == 1 && kill(pid, 0) == 0) {
                fclose(f);
                return 1;
            }
            fclose(f);
        }
        unlink(pp);
    }
    if (!opts->foreground && opts->daemon) daemonize();
    if (write_pid_file(pp) < 0) return 1;
    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL); sigaction(SIGUSR1, &sa, NULL);
    g_config.foreground = opts->foreground; g_config.verbose = opts->verbose;
    config_load(cp, &g_config); logger_init(); log_set_level(opts->log_level);
    if (opts->no_color) log_set_color(0);
    if (epoll_init() < 0 || netlink_init(NULL, &g_config) < 0) return 1;
    if (g_config.app_proxy_enable) app_filter_init(&g_config);
    fcm_monitor_init(&g_config);
    if (g_config.performance_mode) perf_mode_init(&g_config);
    api_init(&g_api_ctx, &g_config);
    service_ctx_t *svc = malloc(sizeof(service_ctx_t));
    if (!svc || service_init(svc, &g_config) < 0 || service_start(svc) < 0) {
        if (svc) free(svc);
        return 1;
    }
    if (g_config.app_proxy_enable) app_filter_setup(&g_config);
    if (g_config.performance_mode) perf_mode_setup(&g_config);
    run_event_loop(svc, &g_api_ctx);
    service_stop(svc); if (svc) free(svc); fcm_monitor_cleanup(); netlink_cleanup();
    api_cleanup(&g_api_ctx);
    epoll_cleanup(); unlink(pp); logger_close();
    return 0;
}

static int do_stop(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    snprintf(pp, sizeof(pp), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);
    FILE *f = fopen(pp, "r");
    if (!f) return 1;
    int pid; if (fscanf(f, "%d", &pid) != 1) { fclose(f); return 1; } fclose(f);
    if (kill(pid, 0) < 0) { unlink(pp); return 1; }
    if (!confirm_operation("stop", opts->force)) return 0;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++) { if (kill(pid, 0) < 0) { unlink(pp); return 0; } usleep(100000); }
    kill(pid, SIGKILL); unlink(pp);
    return 0;
}

static int do_status(atp_options_t *opts) {
    char cp[SAFE_PATH_MAX];
    if (find_config_file(cp, SAFE_PATH_MAX, opts->config_file) != 0) return 1;
    config_set_defaults(&g_config); config_load(cp, &g_config);
    if (opts->no_color) ui_set_no_color(1);
    ui_init(); service_ctx_t svc = {0}; service_init(&svc, &g_config);
    api_init(&g_api_ctx, &g_config); status_show(&g_config, &svc, &g_api_ctx);
    return 0;
}

static int do_reload(atp_options_t *opts) {
    (void)opts; char pp[SAFE_PATH_MAX];
    snprintf(pp, sizeof(pp), "%s/%s", ATP_DEFAULT_DIR, ATP_PID_FILE);
    FILE *f = fopen(pp, "r");
    if (!f) return 1;
    int pid; if (fscanf(f, "%d", &pid) != 1) { fclose(f); return 1; } fclose(f);
    return kill(pid, SIGHUP) == 0 ? 0 : 1;
}

static int do_version(atp_options_t *o) { (void)o; printf("atpd v%s\n", ATP_VERSION_STRING); return 0; }
static int do_help(atp_options_t *o) { (void)o; printf("Usage: atpd [start|stop|status|reload|version]\n"); return 0; }

typedef struct { const char *n; int (*h)(atp_options_t *); } cmd_t;
static const cmd_t cmds[] = { 
    {"start", do_start}, {"stop", do_stop}, {"status", do_status}, 
    {"reload", do_reload}, {"version", do_version}, {"help", do_help}, {NULL, NULL} 
};

int main(int argc, char *argv[]) {
    atp_options_t opts = {0};
    static struct option lg[] = { {"config",1,0,'c'}, {"force",0,0,'f'}, {"version",0,0,'v'}, {0,0,0,0} };
    int c; while ((c = getopt_long(argc, argv, "c:fv", lg, NULL)) != -1) {
        if (c == 'c') snprintf(opts.config_file, sizeof(opts.config_file), "%s", optarg);
        else if (c == 'f') opts.force = 1;
        else if (c == 'v') return do_version(&opts);
    }
    if (optind >= argc) return do_help(&opts);
    for (int i = 0; cmds[i].n; i++) if (strcmp(cmds[i].n, argv[optind]) == 0) return cmds[i].h(&opts);
    return 1;
}
