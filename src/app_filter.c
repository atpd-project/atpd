#include "app_filter.h"
#include "logger.h"
#include "utils.h"
#include "tproxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <sys/stat.h>

#define PACKAGES_LIST_PATH "/data/system/packages.list"
#define APP_IPSET_NAME "atp_app_uids"

/* Cache for package name -> UID mappings */
typedef struct {
    char package_name[256];
    int uid;
    int user_id;
} package_cache_t;

static package_cache_t *g_package_cache = NULL;
static int g_package_cache_count = 0;
static int g_package_cache_loaded = 0;
static int g_current_uids_count = 0;
static int *g_current_uids = NULL;

/* Forward declarations */
static int app_filter_load_package_cache(void);
static int app_filter_uid_in_list(int uid, int *uid_list, int count);
static int parse_user_id_from_line(const char *line);

int app_filter_init(atp_config_t *cfg) {
    (void)cfg;
    
    /* Load package cache once at startup */
    if (!g_package_cache_loaded) {
        if (app_filter_load_package_cache() < 0) {
            LOG_WARN("Failed to load package cache, per-app filtering may not work");
        }
    }
    
    /* Create ipset for UIDs */
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset create %s bitmap:port range 0-65535 2>/dev/null", APP_IPSET_NAME);
    exec_cmd_simple(cmd, 5);
    
    LOG_DEBUG("App filter initialized with ipset %s", APP_IPSET_NAME);
    return 0;
}

/* Robust parsing of packages.list - handles variable field counts across Android versions */
static int parse_user_id_from_line(const char *line) {
    char *line_copy = strdup(line);
    if (!line_copy) return 0;
    
    char *saveptr;
    char *token;
    int field_count = 0;
    char *last_token = NULL;
    
    /* First pass: count fields */
    token = strtok_r(line_copy, " ", &saveptr);
    while (token) {
        field_count++;
        last_token = token;
        token = strtok_r(NULL, " ", &saveptr);
    }
    
    int user_id = 0;
    
    /* If we have at least 11 fields, the 11th field (index 10) is user_id */
    if (field_count >= 11) {
        free(line_copy);
        line_copy = strdup(line);
        if (!line_copy) return 0;
        
        saveptr = NULL;
        token = strtok_r(line_copy, " ", &saveptr);
        for (int i = 1; i < 11 && token; i++) {
            token = strtok_r(NULL, " ", &saveptr);
        }
        if (token) {
            user_id = atoi(token);
        }
    } else if (field_count == 10 || field_count == 9) {
        /* Older Android versions don't have user_id field */
        user_id = 0;
    } else {
        /* Unexpected format, log for debugging */
        LOG_DEBUG("Unexpected packages.list line format (fields=%d): %s", field_count, line);
        user_id = 0;
    }
    
    free(line_copy);
    return user_id;
}

static int app_filter_load_package_cache(void) {
    FILE *fp = fopen(PACKAGES_LIST_PATH, "r");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", PACKAGES_LIST_PATH, strerror(errno));
        return -1;
    }
    
    /* Get file size for initial allocation estimate (exponential growth strategy) */
    struct stat st;
    int estimated_lines = 500;  /* Default estimate */
    if (fstat(fileno(fp), &st) == 0 && st.st_size > 0) {
        /* Rough estimate: each line is about 100-200 bytes */
        estimated_lines = st.st_size / 100 + 100;
        /* Cap at reasonable maximum */
        if (estimated_lines > 10000) estimated_lines = 10000;
    }
    
    package_cache_t *cache = malloc(sizeof(package_cache_t) * estimated_lines);
    if (!cache) {
        fclose(fp);
        return -1;
    }
    
    int cache_capacity = estimated_lines;
    int cache_count = 0;
    char line[1024];
    
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char pkg_name[256];
        int uid;
        
        if (sscanf(line, "%255s %d", pkg_name, &uid) == 2) {
            int user_id = parse_user_id_from_line(line);
            
            /* Exponential growth: double capacity when needed */
            if (cache_count >= cache_capacity) {
                int new_capacity = cache_capacity * 2;
                package_cache_t *new_cache = realloc(cache, sizeof(package_cache_t) * new_capacity);
                if (!new_cache) {
                    free(cache);
                    fclose(fp);
                    return -1;
                }
                cache = new_cache;
                cache_capacity = new_capacity;
                LOG_DEBUG("Package cache expanded to %d entries", new_capacity);
            }
            
            strncpy(cache[cache_count].package_name, pkg_name, sizeof(cache[cache_count].package_name) - 1);
            cache[cache_count].uid = uid;
            cache[cache_count].user_id = user_id;
            cache_count++;
        }
    }
    
    fclose(fp);
    
    /* Shrink to exact size to save memory (optional) */
    if (cache_count < cache_capacity) {
        package_cache_t *new_cache = realloc(cache, sizeof(package_cache_t) * cache_count);
        if (new_cache) cache = new_cache;
    }
    
    g_package_cache = cache;
    g_package_cache_count = cache_count;
    g_package_cache_loaded = 1;
    
    LOG_DEBUG("Loaded %d package entries into cache (initial estimate: %d, final capacity: %d)", 
              cache_count, estimated_lines, cache_count);
    return 0;
}

static int app_filter_find_package_uid(const char *package_name, int user_id) {
    if (!g_package_cache_loaded) return -1;
    
    for (int i = 0; i < g_package_cache_count; i++) {
        if (strcmp(g_package_cache[i].package_name, package_name) == 0) {
            if (user_id == -1 || g_package_cache[i].user_id == user_id) {
                return g_package_cache[i].uid;
            }
        }
    }
    return -1;
}

static int app_filter_uid_in_list(int uid, int *uid_list, int count) {
    for (int i = 0; i < count; i++) {
        if (uid_list[i] == uid) return 1;
    }
    return 0;
}

static int app_filter_parse_package_list_string(const char *list_str, char ***packages, int *count) {
    if (!list_str || list_str[0] == '\0') {
        *packages = NULL;
        *count = 0;
        return 0;
    }
    
    char *str_copy = strdup(list_str);
    if (!str_copy) return -1;
    
    char **pkg_list = NULL;
    int pkg_count = 0;
    char *token = strtok(str_copy, " ");
    
    while (token) {
        char **new_list = realloc(pkg_list, sizeof(char*) * (pkg_count + 1));
        if (!new_list) {
            for (int i = 0; i < pkg_count; i++) free(pkg_list[i]);
            free(pkg_list);
            free(str_copy);
            return -1;
        }
        pkg_list = new_list;
        pkg_list[pkg_count] = strdup(token);
        pkg_count++;
        token = strtok(NULL, " ");
    }
    
    free(str_copy);
    *packages = pkg_list;
    *count = pkg_count;
    return 0;
}

static void app_filter_free_package_list(char **packages, int count) {
    if (!packages) return;
    for (int i = 0; i < count; i++) {
        free(packages[i]);
    }
    free(packages);
}

static int app_filter_add_uids_to_ipset(int *uids, int count, const char *mode) {
    if (count == 0) return 0;
    (void)mode;
    
    /* Flush existing ipset */
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset flush %s 2>/dev/null", APP_IPSET_NAME);
    exec_cmd_simple(cmd, 5);
    
    /* Calculate required buffer size: each UID takes up to 6 chars (5 digits + space) */
    size_t buf_size = (size_t)count * 6 + 1;
    char *uid_list = malloc(buf_size);
    if (!uid_list) {
        LOG_ERROR("Failed to allocate buffer for %d UIDs", count);
        return -1;
    }
    
    char *ptr = uid_list;
    size_t remaining = buf_size;
    
    for (int i = 0; i < count && remaining > 1; i++) {
        int written = snprintf(ptr, remaining, "%d ", uids[i]);
        if (written <= 0 || (size_t)written >= remaining) {
            LOG_WARN("UID list buffer may be too small, truncating at %d UIDs", i);
            break;
        }
        ptr += written;
        remaining -= written;
    }
    
    /* Remove trailing space */
    if (ptr > uid_list) {
        *(ptr - 1) = '\0';
    }
    
    /* Use a single ipset restore command for all UIDs */
    snprintf(cmd, sizeof(cmd), 
             "printf 'create %s bitmap:port range 0-65535\\n' 2>/dev/null; "
             "for uid in %s; do printf 'add %s $uid\\n'; done | ipset restore -exist 2>/dev/null",
             APP_IPSET_NAME, uid_list, APP_IPSET_NAME);
    exec_cmd_simple(cmd, 10);
    
    free(uid_list);
    LOG_DEBUG("Added %d UIDs to ipset %s", count, APP_IPSET_NAME);
    return 0;
}

int app_filter_resolve_packages(const char *packages_list, int **uids, int *count) {
    char **packages;
    int pkg_count;
    
    if (app_filter_parse_package_list_string(packages_list, &packages, &pkg_count) < 0) {
        return -1;
    }
    
    if (pkg_count == 0) {
        *uids = NULL;
        *count = 0;
        app_filter_free_package_list(packages, pkg_count);
        return 0;
    }
    
    /* Ensure package cache is loaded */
    if (!g_package_cache_loaded) {
        app_filter_load_package_cache();
    }
    
    int *uid_list = malloc(sizeof(int) * pkg_count);
    if (!uid_list) {
        app_filter_free_package_list(packages, pkg_count);
        return -1;
    }
    
    int uid_count = 0;
    for (int i = 0; i < pkg_count; i++) {
        char *pkg = packages[i];
        int user_id = 0;
        
        /* Check for user:package format */
        char *colon = strchr(pkg, ':');
        if (colon) {
            *colon = '\0';
            user_id = atoi(pkg);
            pkg = colon + 1;
        }
        
        int uid = app_filter_find_package_uid(pkg, user_id);
        if (uid > 0 && !app_filter_uid_in_list(uid, uid_list, uid_count)) {
            uid_list[uid_count++] = uid;
            LOG_DEBUG("Resolved package %s (user=%d) to UID %d", pkg, user_id, uid);
        } else if (uid <= 0) {
            LOG_WARN("Failed to resolve package: %s", pkg);
        }
    }
    
    app_filter_free_package_list(packages, pkg_count);
    
    if (uid_count == 0) {
        free(uid_list);
        *uids = NULL;
        *count = 0;
    } else {
        *uids = uid_list;
        *count = uid_count;
    }
    
    return 0;
}

void app_filter_free_uids(int *uids) {
    if (uids) free(uids);
}

static void app_filter_configure_chain(atp_config_t *cfg, int family) {
    const char *chain_name = (family == 4) ? "ATP_APP_0" : "ATP6_APP_0";
    const char *table = "mangle";
    
    /* Flush existing rules in chain */
    tproxy_chain_flush(cfg, family, table, chain_name);
    
    char rule[256];
    if (strcmp(cfg->app_proxy_mode, "blacklist") == 0) {
        /* Blacklist: UIDs in set go to ACCEPT (bypass), others go to RETURN (TPROXY) */
        snprintf(rule, sizeof(rule), "-m set --match-set %s src -j ACCEPT", APP_IPSET_NAME);
        tproxy_rule_add(cfg, family, table, chain_name, rule);
        tproxy_rule_add(cfg, family, table, chain_name, "-j RETURN");
    } else {
        /* Whitelist: UIDs in set go to RETURN (TPROXY), others go to ACCEPT (bypass) */
        snprintf(rule, sizeof(rule), "-m set --match-set %s src -j RETURN", APP_IPSET_NAME);
        tproxy_rule_add(cfg, family, table, chain_name, rule);
        tproxy_rule_add(cfg, family, table, chain_name, "-j ACCEPT");
    }
}

int app_filter_setup(atp_config_t *cfg) {
    if (!cfg->app_proxy_enable) {
        LOG_DEBUG("App filter disabled");
        return 0;
    }
    
    LOG_INFO("Setting up application filter (%s mode)", cfg->app_proxy_mode);
    
    int *uids = NULL;
    int uid_count = 0;
    const char *packages_list = NULL;
    
    if (strcmp(cfg->app_proxy_mode, "blacklist") == 0) {
        packages_list = cfg->bypass_apps_list;
    } else {
        packages_list = cfg->proxy_apps_list;
    }
    
    if (app_filter_resolve_packages(packages_list, &uids, &uid_count) < 0) {
        LOG_ERROR("Failed to resolve packages for app filter");
        return -1;
    }
    
    /* Update cached UIDs */
    if (g_current_uids) {
        free(g_current_uids);
    }
    g_current_uids = uids;
    g_current_uids_count = uid_count;
    
    /* Add UIDs to ipset */
    app_filter_add_uids_to_ipset(uids, uid_count, cfg->app_proxy_mode);
    
    /* Configure IPv4 chain */
    app_filter_configure_chain(cfg, 4);
    
    /* Configure IPv6 chain if enabled */
    if (cfg->proxy_ipv6) {
        app_filter_configure_chain(cfg, 6);
    }
    
    LOG_INFO("App filter configured with %d UIDs using ipset %s (IPv6: %s)", 
             uid_count, APP_IPSET_NAME, cfg->proxy_ipv6 ? "enabled" : "disabled");
    return 0;
}

int app_filter_cleanup(atp_config_t *cfg) {
    if (!cfg->app_proxy_enable) {
        return 0;
    }
    
    /* Flush and destroy ipset */
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset flush %s 2>/dev/null; ipset destroy %s 2>/dev/null", 
             APP_IPSET_NAME, APP_IPSET_NAME);
    exec_cmd_simple(cmd, 5);
    
    /* Flush IPv4 chain */
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_APP_0");
    
    /* Flush IPv6 chain if it exists */
    if (cfg->proxy_ipv6) {
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_APP_0");
    }
    
    /* Free cached UIDs */
    if (g_current_uids) {
        free(g_current_uids);
        g_current_uids = NULL;
        g_current_uids_count = 0;
    }
    
    LOG_INFO("App filter cleaned up");
    return 0;
}

/* Helper function to reload app filter without restarting */
int app_filter_reload(atp_config_t *cfg) {
    LOG_INFO("Reloading app filter configuration");
    app_filter_cleanup(cfg);
    return app_filter_setup(cfg);
}
