#include "atpd_global.h"
#include "app_filter.h"
#include "logger.h"
#include "utils.h"
#include "tproxy.h"
#include "ebpf.h"
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
static int app_filter_check_connection_cached_v4(uint32_t s_ip, uint16_t s_pt, uint32_t d_ip, uint16_t d_pt);
static int app_filter_check_connection_cached_v6(const uint8_t *s_ip, uint16_t s_pt, const uint8_t *d_ip, uint16_t d_pt);

int app_filter_init(atp_config_t *cfg) {
    if (!g_package_cache_loaded) app_filter_load_package_cache();
    if (!cfg->core.dry_run) {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "ipset create %s bitmap:port range 0-65535 -exist 2>/dev/null", APP_IPSET_NAME);
        exec_cmd_simple(cmd, 5);
    }
    if (inet_diag_init() != 0) LOG_WARN("INET_DIAG init failed");
    return 0;
}

static int parse_user_id_from_line(const char *line) {
    char *dup = strdup(line), *s = NULL, *t = strtok_r(dup, " ", &s);
    int cnt = 0;
    while (t) { cnt++; t = strtok_r(NULL, " ", &s); }
    int res = 0;
    if (cnt >= 11) {
        free(dup); dup = strdup(line); s = NULL; t = strtok_r(dup, " ", &s);
        for (int i = 1; i < 11 && t; i++) t = strtok_r(NULL, " ", &s);
        if (t) res = atoi(t);
    }
    free(dup); return res;
}

static int app_filter_load_package_cache(void) {
    FILE *fp = fopen(PACKAGES_LIST_PATH, "r");
    if (!fp) return -1;
    struct stat st;
    if (fstat(fileno(fp), &st) == 0) g_package_cache_mtime = st.st_mtime;
    int cap = 1000, cnt = 0;
    package_cache_t *c = malloc(sizeof(package_cache_t) * cap);
    if (!c) {
        fclose(fp);
        return -1;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        char name[256]; int uid;
        if (sscanf(buf, "%255s %d", name, &uid) == 2) {
            if (cnt >= cap) {
                cap *= 2;
                package_cache_t *new_c = realloc(c, sizeof(package_cache_t) * cap);
                if (!new_c) {
                    free(c);
                    fclose(fp);
                    return -1;
                }
                c = new_c;
            }
            snprintf(c[cnt].package_name, sizeof(c[cnt].package_name), "%s", name);
            c[cnt].uid = uid;
            c[cnt].user_id = parse_user_id_from_line(buf);
            cnt++;
        }
    }
    fclose(fp);
    pthread_rwlock_wrlock(&g_package_cache_lock);
    package_cache_t *old = g_package_cache;
    g_package_cache = c; g_package_cache_count = cnt; g_package_cache_loaded = 1;
    pthread_rwlock_unlock(&g_package_cache_lock);
    free(old);
    return 0;
}

static int app_filter_cache_stale(void) {
    struct stat st;
    return (stat(PACKAGES_LIST_PATH, &st) == 0 && st.st_mtime != g_package_cache_mtime);
}

int app_filter_refresh_cache(void) { return app_filter_load_package_cache(); }

int app_filter_get_uid_by_package(const char *name, int uid_req) {
    pthread_mutex_lock(&g_package_reload_mutex);
    if (!g_package_cache_loaded || app_filter_cache_stale()) app_filter_load_package_cache();
    pthread_mutex_unlock(&g_package_reload_mutex);
    int res = -1;
    pthread_rwlock_rdlock(&g_package_cache_lock);
    for (int i = 0; i < g_package_cache_count; i++) {
        if (strcmp(g_package_cache[i].package_name, name) == 0 && (uid_req == -1 || g_package_cache[i].user_id == uid_req)) {
            res = g_package_cache[i].uid; break;
        }
    }
    pthread_rwlock_unlock(&g_package_cache_lock);
    return res;
}

static int app_filter_uid_in_list(int uid, int *list, int count) {
    for (int i = 0; i < count; i++) if (list[i] == uid) return 1;
    return 0;
}

static int app_filter_parse_package_list_string(const char *str, char ***out, int *cnt) {
    if (!str || !*str) { *out = NULL; *cnt = 0; return 0; }
    char *dup = strdup(str), **l = NULL; int n = 0;
    char *t = strtok(dup, " ");
    while (t) {
        l = realloc(l, sizeof(char*) * (n + 1));
        l[n++] = strdup(t);
        t = strtok(NULL, " ");
    }
    free(dup); *out = l; *cnt = n; return 0;
}

static void app_filter_free_package_list(char **l, int n) {
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
}

static int app_filter_add_uids_to_ipset(atp_config_t *cfg, int *uids, int count, const char *mode) {
    (void)cfg; (void)mode;
    char path[] = "/tmp/atp_uids.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    dprintf(fd, "flush %s\n", APP_IPSET_NAME);
    for (int i = 0; i < count; i++) dprintf(fd, "add %s %d\n", APP_IPSET_NAME, uids[i]);
    close(fd);
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset restore < %s 2>/dev/null", path);
    int ret = exec_cmd_simple(cmd, 10);
    unlink(path); return ret;
}

int app_filter_resolve_packages(const char *list, int **uids, int *count) {
    if (!list) { *uids = NULL; *count = 0; return 0; }
    char **p; int n;
    app_filter_parse_package_list_string(list, &p, &n);
    int *l = malloc(sizeof(int) * n), c = 0;
    for (int i = 0; i < n; i++) {
        char *pkg = p[i], *col = strchr(pkg, ':');
        int uid_r = 0;
        if (col) { *col = '\0'; uid_r = atoi(pkg); pkg = col + 1; }
        int uid = app_filter_get_uid_by_package(pkg, uid_r);
        if (uid > 0 && !app_filter_uid_in_list(uid, l, c)) l[c++] = uid;
    }
    app_filter_free_package_list(p, n);
    *uids = l; *count = c; return 0;
}

void app_filter_free_uids(int *uids) { free(uids); }

static int app_filter_check_connection_cached_v4(uint32_t s_ip, uint16_t s_pt, uint32_t d_ip, uint16_t d_pt) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_conn_cache_mutex);
    for (int i = 0; i < g_conn_cache_count; i++) {
        int idx = (g_conn_cache_head + i) % CONN_CACHE_SIZE;
        if (g_conn_cache[idx].src_ip == s_ip && g_conn_cache[idx].src_port == s_pt &&
            g_conn_cache[idx].dst_ip == d_ip && g_conn_cache[idx].dst_port == d_pt &&
            (now - g_conn_cache[idx].timestamp) < CONN_CACHE_TTL) {
            int uid = g_conn_cache[idx].uid;
            pthread_mutex_unlock(&g_conn_cache_mutex); return uid;
        }
    }
    pthread_mutex_unlock(&g_conn_cache_mutex);
    int uid = inet_diag_get_uid_v4(IPPROTO_TCP, s_ip, s_pt, d_ip, d_pt);
    pthread_mutex_lock(&g_conn_cache_mutex);
    int idx = (g_conn_cache_head + g_conn_cache_count) % CONN_CACHE_SIZE;
    if (g_conn_cache_count >= CONN_CACHE_SIZE) g_conn_cache_head = (g_conn_cache_head + 1) % CONN_CACHE_SIZE;
    else g_conn_cache_count++;
    g_conn_cache[idx] = (conn_cache_t){s_ip, s_pt, d_ip, d_pt, uid, now};
    pthread_mutex_unlock(&g_conn_cache_mutex); return uid;
}

static int app_filter_check_connection_cached_v6(const uint8_t *s_ip, uint16_t s_pt, const uint8_t *d_ip, uint16_t d_pt) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_conn_cache_mutex);
    for (int i = 0; i < g_conn_cache_v6_count; i++) {
        int idx = (g_conn_cache_v6_head + i) % CONN_CACHE_SIZE;
        if (memcmp(g_conn_cache_v6[idx].src_ip, s_ip, 16) == 0 && g_conn_cache_v6[idx].src_port == s_pt &&
            memcmp(g_conn_cache_v6[idx].dst_ip, d_ip, 16) == 0 && g_conn_cache_v6[idx].dst_port == d_pt &&
            (now - g_conn_cache_v6[idx].timestamp) < CONN_CACHE_TTL) {
            int uid = g_conn_cache_v6[idx].uid;
            pthread_mutex_unlock(&g_conn_cache_mutex); return uid;
        }
    }
    pthread_mutex_unlock(&g_conn_cache_mutex);
    int uid = inet_diag_get_uid_v6(IPPROTO_TCP, s_ip, s_pt, d_ip, d_pt);
    pthread_mutex_lock(&g_conn_cache_mutex);
    int idx = (g_conn_cache_v6_head + g_conn_cache_v6_count) % CONN_CACHE_SIZE;
    if (g_conn_cache_v6_count >= CONN_CACHE_SIZE) g_conn_cache_v6_head = (g_conn_cache_v6_head + 1) % CONN_CACHE_SIZE;
    else g_conn_cache_v6_count++;
    memcpy(g_conn_cache_v6[idx].src_ip, s_ip, 16);
    g_conn_cache_v6[idx].src_port = s_pt;
    memcpy(g_conn_cache_v6[idx].dst_ip, d_ip, 16);
    g_conn_cache_v6[idx].dst_port = d_pt;
    g_conn_cache_v6[idx].uid = uid;
    g_conn_cache_v6[idx].timestamp = now;
    pthread_mutex_unlock(&g_conn_cache_mutex); return uid;
}

int app_filter_should_proxy(int family, int protocol, void *s_ip, uint16_t s_pt, void *d_ip, uint16_t d_pt) {
    if (protocol != IPPROTO_TCP) return 1;
    int uid = (family == AF_INET) ? app_filter_check_connection_cached_v4(*(uint32_t*)s_ip, s_pt, *(uint32_t*)d_ip, d_pt) : 
                                    app_filter_check_connection_cached_v6((uint8_t*)s_ip, s_pt, (uint8_t*)d_ip, d_pt);
    if (uid <= 0) return 1;
    pthread_rwlock_rdlock(&g_uid_list_lock);
    int in = app_filter_uid_in_list(uid, g_current_uids, g_current_uids_count);
    pthread_rwlock_unlock(&g_uid_list_lock);
    return (strcmp(g_config.filter.app_proxy_mode, "blacklist") == 0) ? !in : in;
}

static void app_filter_configure_chain(atp_config_t *cfg, int family) {
    char ch[64];
    build_chain_name(family, "APP_0", ch, 64);
    tproxy_chain_flush(cfg, family, "mangle", ch);
    char r[256];
    snprintf(r, 256, "-m set --match-set %s src -j %s", APP_IPSET_NAME, 
             strcmp(cfg->filter.app_proxy_mode, "blacklist") == 0 ? "ACCEPT" : "RETURN");
    tproxy_rule_add(cfg, family, "mangle", ch, r);
    tproxy_rule_add(cfg, family, "mangle", ch, strcmp(cfg->filter.app_proxy_mode, "blacklist") == 0 ? "-j RETURN" : "-j ACCEPT");
}

int app_filter_setup(atp_config_t *cfg) {
    if (!cfg->filter.app_proxy_enable) return 0;
    int *uids, c;
    if (app_filter_resolve_packages(cfg->filter.proxy_apps_list, &uids, &c) < 0) return -1;
    pthread_rwlock_wrlock(&g_uid_list_lock);
    free(g_current_uids); g_current_uids = uids; g_current_uids_count = c;
    pthread_rwlock_unlock(&g_uid_list_lock);
    if (!ebpf_is_pure_mode(cfg)) {
        app_filter_add_uids_to_ipset(cfg, uids, c, cfg->filter.app_proxy_mode);
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
    return 0;
}

int app_filter_reload(atp_config_t *cfg) {
    app_filter_refresh_cache();
    pthread_mutex_lock(&g_conn_cache_mutex);
    memset(g_conn_cache, 0, sizeof(g_conn_cache)); memset(g_conn_cache_v6, 0, sizeof(g_conn_cache_v6));
    g_conn_cache_count = g_conn_cache_v6_count = g_conn_cache_head = g_conn_cache_v6_head = 0;
    pthread_mutex_unlock(&g_conn_cache_mutex);
    app_filter_cleanup(cfg); return app_filter_setup(cfg);
}
