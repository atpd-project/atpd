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

#define PACKAGES_LIST_PATH "/data/system/packages.list"

typedef struct {
    char package_name[256];
    int uid;
    int user_id;
} package_entry_t;

static int parse_packages_list(package_entry_t **entries, int *count) {
    FILE *fp = fopen(PACKAGES_LIST_PATH, "r");
    if (!fp) {
        LOG_ERROR("Failed to open %s: %s", PACKAGES_LIST_PATH, strerror(errno));
        return -1;
    }
    
    package_entry_t *list = NULL;
    int entry_count = 0;
    char line[1024];
    
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        /* Format: package_name uid debuggable profileable versionCode targetSdk user_id */
        char pkg_name[256];
        int uid;
        int user_id = 0;
        
        /* Parse at least package name and uid */
        if (sscanf(line, "%255s %d", pkg_name, &uid) == 2) {
            /* Try to parse user_id if present (11th field for Android 11+) */
            char *saveptr;
            char *token = strtok_r(line, " ", &saveptr);
            int field = 0;
            while (token && field < 10) {
                token = strtok_r(NULL, " ", &saveptr);
                field++;
            }
            if (token) {
                user_id = atoi(token);
            }
            
            package_entry_t *new_list = realloc(list, sizeof(package_entry_t) * (entry_count + 1));
            if (!new_list) {
                free(list);
                fclose(fp);
                return -1;
            }
            list = new_list;
            strncpy(list[entry_count].package_name, pkg_name, sizeof(list[entry_count].package_name) - 1);
            list[entry_count].uid = uid;
            list[entry_count].user_id = user_id;
            entry_count++;
        }
    }
    
    fclose(fp);
    *entries = list;
    *count = entry_count;
    return 0;
}

static int find_package_uid(const char *package_name, int user_id, package_entry_t *entries, int entry_count) {
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].package_name, package_name) == 0) {
            if (user_id == -1 || entries[i].user_id == user_id) {
                return entries[i].uid;
            }
        }
    }
    return -1;
}

int app_filter_get_uid_by_package(const char *package_name, int user_id) {
    package_entry_t *entries;
    int entry_count;
    
    if (parse_packages_list(&entries, &entry_count) < 0) {
        return -1;
    }
    
    int uid = find_package_uid(package_name, user_id, entries, entry_count);
    free(entries);
    
    return uid;
}

static int parse_package_list_string(const char *list_str, char ***packages, int *count) {
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

static void free_package_list(char **packages, int count) {
    if (!packages) return;
    for (int i = 0; i < count; i++) {
        free(packages[i]);
    }
    free(packages);
}

int app_filter_resolve_packages(const char *packages_list, int **uids, int *count) {
    char **packages;
    int pkg_count;
    
    if (parse_package_list_string(packages_list, &packages, &pkg_count) < 0) {
        return -1;
    }
    
    if (pkg_count == 0) {
        *uids = NULL;
        *count = 0;
        free_package_list(packages, pkg_count);
        return 0;
    }
    
    package_entry_t *entries;
    int entry_count;
    if (parse_packages_list(&entries, &entry_count) < 0) {
        free_package_list(packages, pkg_count);
        return -1;
    }
    
    int *uid_list = malloc(sizeof(int) * pkg_count);
    if (!uid_list) {
        free_package_list(packages, pkg_count);
        free(entries);
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
        
        int uid = find_package_uid(pkg, user_id, entries, entry_count);
        if (uid > 0) {
            uid_list[uid_count++] = uid;
            LOG_DEBUG("Resolved package %s (user=%d) to UID %d", pkg, user_id, uid);
        } else {
            LOG_WARN("Failed to resolve package: %s", pkg);
        }
    }
    
    free(entries);
    free_package_list(packages, pkg_count);
    
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
        LOG_INFO("Blacklist mode: bypassing %d apps", uid_count);
    } else {
        packages_list = cfg->proxy_apps_list;
        LOG_INFO("Whitelist mode: proxying %d apps", uid_count);
    }
    
    if (app_filter_resolve_packages(packages_list, &uids, &uid_count) < 0) {
        LOG_ERROR("Failed to resolve packages for app filter");
        return -1;
    }
    
    /* Flush existing rules */
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_APP_0");
    
    /* Add rules for each UID */
    for (int i = 0; i < uid_count; i++) {
        char rule[128];
        if (strcmp(cfg->app_proxy_mode, "blacklist") == 0) {
            /* Blacklist: bypassed apps go to ACCEPT, others go to TPROXY */
            snprintf(rule, sizeof(rule), "-m owner --uid-owner %d -j ACCEPT", uids[i]);
            tproxy_rule_add(cfg, 4, "mangle", "ATP_APP_0", rule);
        } else {
            /* Whitelist: proxied apps go to RETURN (continue to TPROXY) */
            snprintf(rule, sizeof(rule), "-m owner --uid-owner %d -j RETURN", uids[i]);
            tproxy_rule_add(cfg, 4, "mangle", "ATP_APP_0", rule);
        }
    }
    
    /* Add default rule */
    if (strcmp(cfg->app_proxy_mode, "blacklist") == 0) {
        /* Blacklist: default is to proxy (RETURN -> TPROXY) */
        tproxy_rule_add(cfg, 4, "mangle", "ATP_APP_0", "-j RETURN");
    } else {
        /* Whitelist: default is to bypass (ACCEPT) */
        tproxy_rule_add(cfg, 4, "mangle", "ATP_APP_0", "-j ACCEPT");
    }
    
    app_filter_free_uids(uids);
    return 0;
}

int app_filter_cleanup(atp_config_t *cfg) {
    if (!cfg->app_proxy_enable) {
        return 0;
    }
    
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_APP_0");
    LOG_INFO("App filter cleaned up");
    return 0;
}

int app_filter_init(atp_config_t *cfg) {
    (void)cfg;
    LOG_DEBUG("App filter initialized");
    return 0;
}
