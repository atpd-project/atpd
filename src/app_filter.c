#include "atpd_global.h"
#include "app_filter.h"
#include "logger.h"
#include "utils.h"
#include "tproxy.h"
#include "inet_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#define PACKAGES_LIST_PATH "/data/system/packages.list"
#define APP_IPSET_NAME "atp_app_uids"
#define CONN_CACHE_SIZE 1024
#define CONN_CACHE_TTL 5

typedef struct {
    char package_name[256];
    int uid;
    int user_id;
} package_cache_t;

static package_cache_t *g_package_cache = NULL;
static int g_package_cache_count = 0;
static int g_package_cache_loaded = 0;
static time_t g_package_cache_mtime = 0;
static int g_package_cache_version = 0;
int g_current_uids_count = 0;
int *g_current_uids = NULL;

static pthread_rwlock_t g_package_cache_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t g_package_reload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_uid_list_lock = PTHREAD_RWLOCK_INITIALIZER;

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    int uid;
    time_t timestamp;
} conn_cache_t;

typedef struct {
    uint8_t src_ip[16];
    uint16_t src_port;
    uint8_t dst_ip[16];
    uint16_t dst_port;
    int uid;
    time_t timestamp;
} conn_cache_v6_t;

static conn_cache_t g_conn_cache[CONN_CACHE_SIZE];
static conn_cache_v6_t g_conn_cache_v6[CONN_CACHE_SIZE];
static unsigned int g_conn_cache_head = 0;
static unsigned int g_conn_cache_v6_head = 0;
static int g_conn_cache_count = 0;
static int g_conn_cache_v6_count = 0;
static pthread_mutex_t g_conn_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static int app_filter_load_package_cache(void);
static int app_filter_uid_in_list(int uid, int *uid_list, int count);
static int parse_user_id_from_line(const char *line);
static int app_filter_cache_stale(void);

int app_filter_init(atp_config_t *cfg) {
    (void)cfg;
    if (!g_package_cache_loaded) {
        if (app_filter_load_package_cache() < 0) {
            LOG_WARN("Failed to load package cache");
        }
    }
    if (!cfg->core.dry_run) {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "ipset create %s bitmap:port range 0-65535 -exist 2>/dev/null", APP_IPSET_NAME);
        exec_cmd_simple(cmd, 5);
    }
    if (inet_diag_init() != 0) {
        LOG_WARN("INET_DIAG initialization failed");
    }
    return 0;
}

static int parse_user_id_from_line(const char *line) {
    char *line_copy = strdup(line);
    if (!line_copy) return 0;
    char *saveptr, *token;
    int field_count = 0;
    token = strtok_r(line_copy, " ", &saveptr);
    while (token) { field_count++; token = strtok_r(NULL, " ", &saveptr); }
    int user_id = 0;
    if (field_count >= 11) {
        free(line_copy);
        line_copy = strdup(line);
        saveptr = NULL;
        token = strtok_r(line_copy, " ", &saveptr);
        for (int i = 1; i < 11 && token; i++) token = strtok_r(NULL, " ", &saveptr);
        if (token) {
            char *endptr;
            long parsed = strtol(token, &endptr, 10);
            if (endptr != token && *endptr == '\0') user_id = (int)parsed;
        }
    }
    free(line_copy);
    return user_id;
}

static int app_filter_load_package_cache(void) {
    FILE *fp = fopen(PACKAGES_LIST_PATH, "r");
    if (!fp) return -1;
    struct stat st;
    if (fstat(fileno(fp), &st) == 0) g_package_cache_mtime = st.st_mtime;
    int cache_capacity = 1000;
    package_cache_t *cache = malloc(sizeof(package_cache_t) * cache_capacity);
    if (!cache) { fclose(fp); return -1; }
    int cache_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char pkg_name[256];
        int uid;
        if (sscanf(line, "%255s %d", pkg_name, &uid) == 2) {
            if (cache_count >= cache_capacity) {
                cache_capacity *= 2;
                cache = realloc(cache, sizeof(package_cache_t) * cache_capacity);
            }
            snprintf(cache[cache_count].package_name, 256, "%s", pkg_name);
            cache[cache_count].uid = uid;
            cache[cache_count].user_id = parse_user_id_from_line(line);
            cache_count++;
        }
    }
    fclose(fp);
    pthread_rwlock_wrlock(&g_package_cache_lock);
    package_cache_t *old = g_package_cache;
    g_package_cache = cache;
    g_package_cache_count = cache_count;
    g_package_cache_loaded = 1;
    g_package_cache_version++;
    pthread_rwlock_unlock(&g_package_cache_lock);
    free(old);
    return 0;
}

static int app_filter_cache_stale(void) {
    struct stat st;
    if (stat(PACKAGES_LIST_PATH, &st) != 0) return 1;
    return st.st_mtime != g_package_cache_mtime;
}

int app_filter_refresh_cache(void) { return app_filter_load_package_cache(); }

static int app_filter_find_package_uid(const char *package_name, int user_id) {
    int result = -1;
    pthread_rwlock_rdlock(&g_package_cache_lock);
    for (int i = 0; i < g_package_cache_count; i++) {
        if (strcmp(g_package_cache[i].package_name, package_name) == 0 &&
            (user_id == -1 || g_package_cache[i].user_id == user_id)) {
            result = g_package_cache[i].uid;
            break;
        }
    }
    pthread_rwlock_unlock(&g_package_cache_lock);
    return result;
}

int app_filter_get_uid_by_package(const char *package_name, int user_id) {
    pthread_mutex_lock(&g_package_reload_mutex);
    if (!g_package_cache_loaded || app_filter_cache_stale()) app_filter_load_package_cache();
    pthread_mutex_unlock(&g_package_reload_mutex);
    return app_filter_find_package_uid(package_name, user_id);
}

static int app_filter_uid_in_list(int uid, int *uid_list, int count) {
    for (int i = 0; i < count; i++) if (uid_list[i] == uid) return 1;
    return 0;
}

static int app_filter_parse_package_list_string(const char *list_str, char ***packages, int *count) {
    if (!list_str || !list_str[0]) { *packages = NULL; *count = 0; return 0; }
    char *str_copy = strdup(list_str), **pkg_list = NULL;
    int pkg_count = 0;
    char *token = strtok(str_copy, " ");
    while (token) {
        pkg_list = realloc(pkg_list, sizeof(char*) * (pkg_count + 1));
        pkg_list[pkg_count++] = strdup(token);
        token = strtok(NULL, " ");
    }
    free(str_copy);
    *packages = pkg_list; *count = pkg_count;
    return 0;
}

static void app_filter_free_package_list(char **packages, int count) {
    for (int i = 0; i < count; i++) free(packages[i]);
    free(packages);
}

static int app_filter_add_uids_to_ipset(atp_config_t *cfg, int *uids, int count, const char *mode) {
    (void)cfg; (void)mode;
    char tmp_path[] = "/tmp/atp_uids.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;
    dprintf(fd, "flush %s\n", APP_IPSET_NAME);
    for (int i = 0; i < count; i++) dprintf(fd, "add %s %d\n", APP_IPSET_NAME, uids[i]);
    close(fd);
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset restore < %s 2>/dev/null", tmp_path);
    int ret = exec_cmd_simple(cmd, 10);
    unlink(tmp_path);
    return ret;
}

int app_filter_resolve_packages(const char *list, int **uids, int *count) {
    if (!list) { *uids = NULL; *count = 0; return 0; }
    char **packages; int pkg_count;
    if (app_filter_parse_package_list_string(list, &packages, &pkg_count) < 0) return -1;
    int *uid_list = malloc(sizeof(int) * pkg_count), uid_count = 0;
    for (int i = 0; i < pkg_count; i++) {
        char *pkg = packages[i]; int user_id = 0, *colon = strchr(pkg, ':');
        if (colon) { *colon = '\0'; user_id = atoi(pkg); pkg = colon + 1; }
        int uid = app_filter_get_uid_by_package(pkg, user_id);
        if (uid > 0 && !app_filter_uid_in_list(uid, uid_list, uid_count)) uid_list[uid_count++] = uid;
    }
    app_filter_free_package_list(packages, pkg_count);
    *uids = uid_list; *count = uid_count;
    return 0;
}

void app_filter_free_uids(int *uids) { free(uids); }

static int app_filter_check_connection_cached_v4(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_conn_cache_mutex);
    for (int i = 0; i < g_conn_cache_count; i++) {
        int idx = (g_conn_cache_head + i) % CONN_CACHE_SIZE;
        if (g_conn_cache[idx].src_ip == src_ip && g_conn_cache[idx].src_port == src_port &&
            g_conn_cache[idx].dst_ip == dst_ip && g_conn_cache[idx].dst_port == dst_port &&
            (now - g_conn_cache[idx].timestamp) < CONN_CACHE_TTL) {
            int uid = g_conn_cache[idx].uid;
            pthread_mutex_unlock(&g_conn_cache_mutex);
            return uid;
        }
    }
    pthread_mutex_unlock(&g_conn_cache_mutex);
    int uid = inet_diag_get_uid_v4(IPPROTO_TCP, src_ip, src_port, dst_ip, dst_port);
    pthread_mutex_lock(&g_conn_cache_mutex);
    int idx = (g_conn_cache_head + g_conn_cache_count) % CONN_CACHE_SIZE;
    if (g_conn_cache_count >= CONN_CACHE_SIZE) g_conn_cache_head = (g_conn_cache_head + 1) % CONN_CACHE_SIZE;
    else g_conn_cache_count++;
    g_conn_cache[idx] = (conn_cache_t){src_ip, src_port, dst_ip, dst_port, uid, now};
    pthread_mutex_unlock(&g_conn_cache_mutex);
    return uid;
}

int app_filter_should_proxy(int family, int protocol, void *src_ip, uint16_t src_port, void *dst_ip, uint16_t dst_port) {
    if (protocol != IPPROTO_TCP) return 1;
    int uid = (family == AF_INET) ? 
              app_filter_check_connection_cached_v4(*(uint32_t*)src_ip, src_port, *(uint32_t*)dst_ip, dst_port) : 
              app_filter_check_connection_cached_v6((uint8_t*)src_ip, src_port, (uint8_t*)dst_ip, dst_port);
    if (uid <= 0) return 1;
    pthread_rwlock_rdlock(&g_uid_list_lock);
    int in_list = app_filter_uid_in_list(uid, g_current_uids, g_current_uids_count);
    pthread_rwlock_unlock(&g_uid_list_lock);
    return (strcmp(g_config.filter.app_proxy_mode, "blacklist") == 0) ? !in_list : in_list;
}

static void app_filter_configure_chain(atp_config_t *cfg, int family) {
    char chain[64];
    build_chain_name(family, "APP_0", chain, 64);
    tproxy_chain_flush(cfg, family, "mangle", chain);
    char rule[256];
    snprintf(rule, sizeof(rule), "-m set --match-set %s src -j %s", APP_IPSET_NAME, 
             strcmp(cfg->filter.app_proxy_mode, "blacklist") == 0 ? "ACCEPT" : "RETURN");
    tproxy_rule_add(cfg, family, "mangle", chain, rule);
    tproxy_rule_add(cfg, family, "mangle", chain, 
                    strcmp(cfg->filter.app_proxy_mode, "blacklist") == 0 ? "-j RETURN" : "-j ACCEPT");
}

int app_filter_setup(atp_config_t *cfg) {
    if (!cfg->filter.app_proxy_enable) return 0;
    int *uids, uid_count;
    if (app_filter_resolve_packages(cfg->filter.proxy_apps_list, &uids, &uid_count) < 0) return -1;
    pthread_rwlock_wrlock(&g_uid_list_lock);
    free(g_current_uids); g_current_uids = uids; g_current_uids_count = uid_count;
    pthread_rwlock_unlock(&g_uid_list_lock);
    if (!cfg->ebpf.ready) {
        app_filter_add_uids_to_ipset(cfg, uids, uid_count, cfg->filter.app_proxy_mode);
        app_filter_configure_chain(cfg, 4);
        if (cfg->network.proxy_ipv6) app_filter_configure_chain(cfg, 6);
    }
    return 0;
}

int app_filter_cleanup(atp_config_t *cfg) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset flush %s 2>/dev/null; ipset destroy %s 2>/dev/null", APP_IPSET_NAME, APP_IPSET_NAME);
    exec_cmd_simple(cmd, 5);
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_APP_0");
    if (cfg->network.proxy_ipv6) tproxy_chain_flush(cfg, 6, "mangle", "ATP6_APP_0");
    pthread_rwlock_wrlock(&g_uid_list_lock);
    free(g_current_uids); g_current_uids = NULL; g_current_uids_count = 0;
    pthread_rwlock_unlock(&g_uid_list_lock);
    inet_diag_cleanup();
    return 0;
}

int app_filter_reload(atp_config_t *cfg) {
    app_filter_refresh_cache();
    pthread_mutex_lock(&g_conn_cache_mutex);
    memset(g_conn_cache, 0, sizeof(g_conn_cache));
    g_conn_cache_count = 0;
    pthread_mutex_unlock(&g_conn_cache_mutex);
    app_filter_cleanup(cfg);
    return app_filter_setup(cfg);
}
