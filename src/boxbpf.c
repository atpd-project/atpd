#include "boxbpf.h"
#include "bpf_common.h"
#include "logger.h"
#include "utils.h"
#include "app_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

static bool g_ebpf_ready = false;
static char g_pin_dir[256] = "/sys/fs/bpf/box";

static void raise_memlock(void) {
    struct rlimit unlimited = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &unlimited);
}

struct fds {
    int cidr4;
    int cidr6;
    int force_uid;
    int app_uid;
};

static void init_fds(struct fds *fds) {
    fds->cidr4 = -1;
    fds->cidr6 = -1;
    fds->force_uid = -1;
    fds->app_uid = -1;
}

static void close_fds(struct fds *fds) {
    close_fd(fds->cidr4);
    close_fd(fds->cidr6);
    close_fd(fds->force_uid);
    close_fd(fds->app_uid);
    init_fds(fds);
}

static int create_cidr_map(const char *source, const char *pin_path,
                           bool ipv6, int *fd_out) {
    *fd_out = create_map(
        BPF_MAP_TYPE_LPM_TRIE,
        ipv6 ? sizeof(struct lpm6_key) : sizeof(struct lpm4_key),
        sizeof(uint8_t),
        MAX_CIDRS,
        COMPAT_MAP_NO_PREALLOC
    );
    if (*fd_out < 0) {
        LOG_ERROR("create IPv%d CIDR map failed", ipv6 ? 6 : 4);
        return -1;
    }

    load_cidr_file(*fd_out, source, ipv6);
    return pin_replace(*fd_out, pin_path) == 0 ? 0 : -1;
}

static int create_uid_map(const char *source, const char *pin_path,
                          bool required, int *fd_out) {
    *fd_out = create_map(BPF_MAP_TYPE_HASH, sizeof(uint32_t),
                         sizeof(uint8_t), MAX_UIDS, 0);
    if (*fd_out < 0) {
        LOG_ERROR("create UID map failed");
        return -1;
    }

    int loaded = load_uid_file(*fd_out, source);
    if (required && loaded <= 0) {
        LOG_ERROR("required UID map is empty: %s", source ? source : "-");
        return -1;
    }

    return pin_replace(*fd_out, pin_path) == 0 ? 0 : -1;
}

static int pin_program(const char *section, const char *name,
                       const char *pin_path, const struct fds *fds) {
    int fd = load_program(section, name, fds->cidr4, fds->cidr6,
                          fds->force_uid, fds->app_uid);
    if (fd < 0) return -1;

    int rc = pin_replace(fd, pin_path);
    close(fd);
    return rc == 0 ? 0 : -1;
}

static int write_ebpf_config(atp_config_t *cfg) {
    if (!cfg) return -1;

    char *state_dir = cfg->ebpf_state_dir;
    char *pin_dir = cfg->ebpf_pin_dir;
    char config_path[512];
    char empty_v4[512], empty_v6[512], force_uids[512], app_uids[512];

    snprintf(config_path, sizeof(config_path), "%s/config.json", state_dir);
    snprintf(empty_v4, sizeof(empty_v4), "%s/empty-v4.txt", state_dir);
    snprintf(empty_v6, sizeof(empty_v6), "%s/empty-v6.txt", state_dir);
    snprintf(force_uids, sizeof(force_uids), "%s/force-uids.txt", state_dir);
    snprintf(app_uids, sizeof(app_uids), "%s/app-uids.txt", state_dir);

    mkdir_recursive(state_dir, 0755);
    mkdir_recursive(pin_dir, 0755);

    FILE *f = fopen(empty_v4, "w");
    if (f) fclose(f);
    f = fopen(empty_v6, "w");
    if (f) fclose(f);

    FILE *fu = fopen(force_uids, "w");
    if (fu) {
        if (cfg->cnip_force_proxy_apps[0] != '\0') {
            char *copy = strdup(cfg->cnip_force_proxy_apps);
            if (copy) {
                char *token = strtok(copy, " ");
                while (token) {
                    int uid = app_filter_get_uid_by_package(token, 0);
                    if (uid > 0) {
                        fprintf(fu, "%d\n", uid);
                    }
                    token = strtok(NULL, " ");
                }
                free(copy);
            }
        }
        fclose(fu);
    }

    FILE *au = fopen(app_uids, "w");
    if (au) {
        if (cfg->performance_mode && cfg->app_proxy_enable) {
            const char *pkg_list = NULL;
            if (strcmp(cfg->app_proxy_mode, "blacklist") == 0) {
                pkg_list = cfg->bypass_apps_list;
            } else {
                pkg_list = cfg->proxy_apps_list;
            }
            if (pkg_list && pkg_list[0] != '\0') {
                int *uids = NULL;
                int count = 0;
                if (app_filter_resolve_packages(pkg_list, &uids, &count) == 0 && count > 0) {
                    for (int i = 0; i < count; i++) {
                        fprintf(au, "%d\n", uids[i]);
                    }
                    app_filter_free_uids(uids);
                }
            }
        }
        fclose(au);
    }

    char cidr4[512], cidr6[512];
    snprintf(cidr4, sizeof(cidr4), "%s", empty_v4);
    snprintf(cidr6, sizeof(cidr6), "%s", empty_v6);

    if (cfg->bypass_cn_ip) {
        char cn_path[512];
        snprintf(cn_path, sizeof(cn_path), "%s/%s", cfg->data_dir, cfg->cn_ip_file);
        if (file_exists(cn_path)) {
            strncpy(cidr4, cn_path, sizeof(cidr4) - 1);
            cidr4[sizeof(cidr4) - 1] = '\0';
        }
        if (cfg->proxy_ipv6) {
            snprintf(cn_path, sizeof(cn_path), "%s/%s", cfg->data_dir, cfg->cn_ipv6_file);
            if (file_exists(cn_path)) {
                strncpy(cidr6, cn_path, sizeof(cidr6) - 1);
                cidr6[sizeof(cidr6) - 1] = '\0';
            }
        }
    }

    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        LOG_ERROR("Failed to create eBPF config: %s", config_path);
        return -1;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"ipv6\": %s,\n", cfg->proxy_ipv6 ? "true" : "false");
    fprintf(fp, "  \"cidr4\": \"%s\",\n", cidr4);
    fprintf(fp, "  \"cidr6\": \"%s\",\n", cidr6);
    fprintf(fp, "  \"forceUids\": \"%s\",\n", force_uids);
    fprintf(fp, "  \"appUids\": \"%s\",\n", app_uids);
    fprintf(fp, "  \"pinCidrOut4\": \"%s/box_cidr_out4\",\n", pin_dir);
    fprintf(fp, "  \"pinCidrOut6\": \"%s/box_cidr_out6\",\n", pin_dir);
    fprintf(fp, "  \"pinCidrPre4\": \"%s/box_cidr_pre4\",\n", pin_dir);
    fprintf(fp, "  \"pinCidrPre6\": \"%s/box_cidr_pre6\",\n", pin_dir);
    fprintf(fp, "  \"pinForceOut4\": \"%s/box_force_out4\",\n", pin_dir);
    fprintf(fp, "  \"pinForceOut6\": \"%s/box_force_out6\",\n", pin_dir);
    fprintf(fp, "  \"pinAppOut4\": \"%s/box_app_out4\",\n", pin_dir);
    fprintf(fp, "  \"pinAppOut6\": \"%s/box_app_out6\",\n", pin_dir);
    fprintf(fp, "  \"mapCidr4\": \"%s/box_cidr4_lpm\",\n", pin_dir);
    fprintf(fp, "  \"mapCidr6\": \"%s/box_cidr6_lpm\",\n", pin_dir);
    fprintf(fp, "  \"mapForceUid\": \"%s/box_force_uid_set\",\n", pin_dir);
    fprintf(fp, "  \"mapAppUid\": \"%s/box_app_uid_set\"\n", pin_dir);
    fprintf(fp, "}\n");

    fclose(fp);
    LOG_INFO("eBPF config written: %s", config_path);
    return 0;
}

static int apply_config(const char *config_path) {
    if (!config_path || access(config_path, R_OK) != 0) {
        LOG_ERROR("eBPF config not found: %s", config_path);
        return -1;
    }

    remove_known_pins();

    int status = -1;
    struct fds fds;
    init_fds(&fds);

    char cidr4_file[512] = {0};
    char cidr6_file[512] = {0};
    char force_uid_file[512] = {0};
    char app_uid_file[512] = {0};
    char pin_cidr_out4[128] = {0};
    char pin_cidr_out6[128] = {0};
    char pin_cidr_pre4[128] = {0};
    char pin_cidr_pre6[128] = {0};
    char pin_force_out4[128] = {0};
    char pin_force_out6[128] = {0};
    char pin_app_out4[128] = {0};
    char pin_app_out6[128] = {0};
    char map_cidr4[128] = {0};
    char map_cidr6[128] = {0};
    char map_force_uid[128] = {0};
    char map_app_uid[128] = {0};
    bool ipv6 = false;

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_ERROR("Failed to open config: %s", config_path);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
        if (*trimmed == '#' || *trimmed == '\0') continue;
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len-1] == '\n' || trimmed[len-1] == '\r')) {
            trimmed[--len] = '\0';
        }
        char *colon = strchr(trimmed, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = trimmed;
        char *value = colon + 1;
        while (*key == ' ' || *key == '\t') ++key;
        while (*value == ' ' || *value == '\t') ++value;

        if (strcmp(key, "ipv6") == 0) {
            ipv6 = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "cidr4") == 0) {
            strncpy(cidr4_file, value, sizeof(cidr4_file) - 1);
        } else if (strcmp(key, "cidr6") == 0) {
            strncpy(cidr6_file, value, sizeof(cidr6_file) - 1);
        } else if (strcmp(key, "forceUids") == 0) {
            strncpy(force_uid_file, value, sizeof(force_uid_file) - 1);
        } else if (strcmp(key, "appUids") == 0) {
            strncpy(app_uid_file, value, sizeof(app_uid_file) - 1);
        } else if (strcmp(key, "pinCidrOut4") == 0) {
            strncpy(pin_cidr_out4, value, sizeof(pin_cidr_out4) - 1);
        } else if (strcmp(key, "pinCidrOut6") == 0) {
            strncpy(pin_cidr_out6, value, sizeof(pin_cidr_out6) - 1);
        } else if (strcmp(key, "pinCidrPre4") == 0) {
            strncpy(pin_cidr_pre4, value, sizeof(pin_cidr_pre4) - 1);
        } else if (strcmp(key, "pinCidrPre6") == 0) {
            strncpy(pin_cidr_pre6, value, sizeof(pin_cidr_pre6) - 1);
        } else if (strcmp(key, "pinForceOut4") == 0) {
            strncpy(pin_force_out4, value, sizeof(pin_force_out4) - 1);
        } else if (strcmp(key, "pinForceOut6") == 0) {
            strncpy(pin_force_out6, value, sizeof(pin_force_out6) - 1);
        } else if (strcmp(key, "pinAppOut4") == 0) {
            strncpy(pin_app_out4, value, sizeof(pin_app_out4) - 1);
        } else if (strcmp(key, "pinAppOut6") == 0) {
            strncpy(pin_app_out6, value, sizeof(pin_app_out6) - 1);
        } else if (strcmp(key, "mapCidr4") == 0) {
            strncpy(map_cidr4, value, sizeof(map_cidr4) - 1);
        } else if (strcmp(key, "mapCidr6") == 0) {
            strncpy(map_cidr6, value, sizeof(map_cidr6) - 1);
        } else if (strcmp(key, "mapForceUid") == 0) {
            strncpy(map_force_uid, value, sizeof(map_force_uid) - 1);
        } else if (strcmp(key, "mapAppUid") == 0) {
            strncpy(map_app_uid, value, sizeof(map_app_uid) - 1);
        }
    }
    fclose(fp);

    if (cidr4_file[0] == '\0') {
        LOG_ERROR("missing cidr4 in config");
        return -1;
    }
    if (ipv6 && cidr6_file[0] == '\0') {
        LOG_ERROR("missing cidr6 in config (ipv6 enabled)");
        return -1;
    }

    if (pin_cidr_out4[0] == '\0') {
        strcpy(pin_cidr_out4, PIN_CIDR_OUT4);
    }
    if (pin_cidr_pre4[0] == '\0') {
        strcpy(pin_cidr_pre4, PIN_CIDR_PRE4);
    }
    if (pin_force_out4[0] == '\0') {
        strcpy(pin_force_out4, PIN_FORCE_OUT4);
    }
    if (pin_app_out4[0] == '\0') {
        strcpy(pin_app_out4, PIN_APP_OUT4);
    }
    if (map_cidr4[0] == '\0') {
        strcpy(map_cidr4, MAP_CIDR4);
    }
    if (map_cidr6[0] == '\0') {
        strcpy(map_cidr6, MAP_CIDR6);
    }
    if (map_force_uid[0] == '\0') {
        strcpy(map_force_uid, MAP_FORCE_UID);
    }
    if (map_app_uid[0] == '\0') {
        strcpy(map_app_uid, MAP_APP_UID);
    }
    if (ipv6) {
        if (pin_cidr_out6[0] == '\0') strcpy(pin_cidr_out6, PIN_CIDR_OUT6);
        if (pin_cidr_pre6[0] == '\0') strcpy(pin_cidr_pre6, PIN_CIDR_PRE6);
        if (pin_force_out6[0] == '\0') strcpy(pin_force_out6, PIN_FORCE_OUT6);
        if (pin_app_out6[0] == '\0') strcpy(pin_app_out6, PIN_APP_OUT6);
    }

    bool app_uid_required = uid_file_has_entries(app_uid_file);
    if (create_cidr_map(cidr4_file, map_cidr4, false, &fds.cidr4) != 0) goto cleanup;
    if (ipv6 && create_cidr_map(cidr6_file, map_cidr6, true, &fds.cidr6) != 0) goto cleanup;
    if (create_uid_map(force_uid_file, map_force_uid, false, &fds.force_uid) != 0) goto cleanup;
    if (create_uid_map(app_uid_file, map_app_uid, app_uid_required, &fds.app_uid) != 0) goto cleanup;

    if (pin_program("socket/cidr4", "cidr4", pin_cidr_out4, &fds) != 0) goto cleanup;
    if (pin_program("socket/cidr4", "pre4", pin_cidr_pre4, &fds) != 0) goto cleanup;
    if (pin_program("socket/force4", "force4", pin_force_out4, &fds) != 0) goto cleanup;
    if (pin_program("socket/appuid", "app4", pin_app_out4, &fds) != 0) goto cleanup;

    if (ipv6) {
        if (pin_program("socket/cidr6", "cidr6", pin_cidr_out6, &fds) != 0) goto cleanup;
        if (pin_program("socket/cidr6", "pre6", pin_cidr_pre6, &fds) != 0) goto cleanup;
        if (pin_program("socket/force6", "force6", pin_force_out6, &fds) != 0) goto cleanup;
        if (pin_program("socket/appuid", "app6", pin_app_out6, &fds) != 0) goto cleanup;
    }

    status = 0;

cleanup:
    if (status != 0) remove_known_pins();
    close_fds(&fds);
    return status;
}

static int update_config(const char *config_path) {
    if (!config_path || access(config_path, R_OK) != 0) {
        LOG_ERROR("eBPF config not found: %s", config_path);
        return -1;
    }

    char map_cidr4[128] = {0};
    char map_cidr6[128] = {0};
    char map_force_uid[128] = {0};
    char map_app_uid[128] = {0};
    char cidr4_file[512] = {0};
    char cidr6_file[512] = {0};
    char force_uid_file[512] = {0};
    char app_uid_file[512] = {0};
    bool ipv6 = false;

    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_ERROR("Failed to open config: %s", config_path);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
        if (*trimmed == '#' || *trimmed == '\0') continue;
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len-1] == '\n' || trimmed[len-1] == '\r')) {
            trimmed[--len] = '\0';
        }
        char *colon = strchr(trimmed, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = trimmed;
        char *value = colon + 1;
        while (*key == ' ' || *key == '\t') ++key;
        while (*value == ' ' || *value == '\t') ++value;

        if (strcmp(key, "ipv6") == 0) {
            ipv6 = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "cidr4") == 0) {
            strncpy(cidr4_file, value, sizeof(cidr4_file) - 1);
        } else if (strcmp(key, "cidr6") == 0) {
            strncpy(cidr6_file, value, sizeof(cidr6_file) - 1);
        } else if (strcmp(key, "forceUids") == 0) {
            strncpy(force_uid_file, value, sizeof(force_uid_file) - 1);
        } else if (strcmp(key, "appUids") == 0) {
            strncpy(app_uid_file, value, sizeof(app_uid_file) - 1);
        } else if (strcmp(key, "mapCidr4") == 0) {
            strncpy(map_cidr4, value, sizeof(map_cidr4) - 1);
        } else if (strcmp(key, "mapCidr6") == 0) {
            strncpy(map_cidr6, value, sizeof(map_cidr6) - 1);
        } else if (strcmp(key, "mapForceUid") == 0) {
            strncpy(map_force_uid, value, sizeof(map_force_uid) - 1);
        } else if (strcmp(key, "mapAppUid") == 0) {
            strncpy(map_app_uid, value, sizeof(map_app_uid) - 1);
        }
    }
    fclose(fp);

    if (map_cidr4[0] == '\0') strcpy(map_cidr4, MAP_CIDR4);
    if (map_cidr6[0] == '\0') strcpy(map_cidr6, MAP_CIDR6);
    if (map_force_uid[0] == '\0') strcpy(map_force_uid, MAP_FORCE_UID);
    if (map_app_uid[0] == '\0') strcpy(map_app_uid, MAP_APP_UID);
    if (cidr4_file[0] == '\0') {
        LOG_ERROR("missing cidr4 in config");
        return -1;
    }

    int cidr4 = get_pinned(map_cidr4);
    int cidr6 = get_pinned(map_cidr6);
    int force_uid = get_pinned(map_force_uid);
    int app_uid = get_pinned(map_app_uid);

    if (cidr4 < 0 || (ipv6 && cidr6 < 0) || force_uid < 0 || app_uid < 0) {
        close_fd(cidr4);
        close_fd(cidr6);
        close_fd(force_uid);
        close_fd(app_uid);
        return apply_config(config_path);
    }

    int status = 0;
    if (clear_map(cidr4, sizeof(struct lpm4_key)) < 0) status = -1;
    load_cidr_file(cidr4, cidr4_file, false);
    if (ipv6) {
        if (clear_map(cidr6, sizeof(struct lpm6_key)) < 0) status = -1;
        load_cidr_file(cidr6, cidr6_file, true);
    }
    if (clear_map(force_uid, sizeof(uint32_t)) < 0) status = -1;
    load_uid_file(force_uid, force_uid_file);
    if (clear_map(app_uid, sizeof(uint32_t)) < 0) status = -1;
    load_uid_file(app_uid, app_uid_file);

    close_fd(cidr4);
    close_fd(cidr6);
    close_fd(force_uid);
    close_fd(app_uid);
    return status;
}

int boxbpf_probe(bool ipv6) {
    raise_memlock();

    char map_cidr4[128];
    char map_cidr6[128];
    char map_force_uid[128];
    char map_app_uid[128];
    snprintf(map_cidr4, sizeof(map_cidr4), "%s", PROBE_MAP_CIDR4);
    snprintf(map_cidr6, sizeof(map_cidr6), "%s", PROBE_MAP_CIDR6);
    snprintf(map_force_uid, sizeof(map_force_uid), "%s", PROBE_MAP_FORCE_UID);
    snprintf(map_app_uid, sizeof(map_app_uid), "%s", PROBE_MAP_APP_UID);

    struct fds fds;
    init_fds(&fds);
    bool ok = false;

    if (create_cidr_map("/dev/null", map_cidr4, false, &fds.cidr4) != 0) goto cleanup;
    if (create_cidr_map("/dev/null", map_cidr6, true, &fds.cidr6) != 0) goto cleanup;
    if (create_uid_map("", map_force_uid, false, &fds.force_uid) != 0) goto cleanup;
    if (create_uid_map("", map_app_uid, false, &fds.app_uid) != 0) goto cleanup;

    ok = pin_program("socket/cidr4", "probe4", PROBE_PIN4, &fds) == 0;
    if (ipv6) {
        ok = pin_program("socket/cidr6", "probe6", PROBE_PIN6, &fds) == 0 && ok;
    }

cleanup:
    close_fds(&fds);
    unlink(PROBE_PIN4);
    unlink(PROBE_PIN6);
    unlink(map_cidr4);
    unlink(map_cidr6);
    unlink(map_force_uid);
    unlink(map_app_uid);
    rmdir(PIN_DIR);

    g_ebpf_ready = ok;
    return ok ? 0 : -1;
}

int boxbpf_apply(const char *config_path) {
    raise_memlock();
    int ret = apply_config(config_path);
    g_ebpf_ready = (ret == 0);
    return ret;
}

int boxbpf_update(const char *config_path) {
    raise_memlock();
    return update_config(config_path);
}

int boxbpf_clear(void) {
    remove_known_pins();
    g_ebpf_ready = false;
    return 0;
}

bool boxbpf_is_ready(void) {
    return g_ebpf_ready;
}

const char *boxbpf_pin_dir(void) {
    return g_pin_dir;
}

int boxbpf_init_from_config(atp_config_t *cfg) {
    if (!cfg) return -1;

    if (!cfg->ebpf_enabled) {
        LOG_DEBUG("eBPF disabled by config");
        return -1;
    }

    if (cfg->cnip_mode != 1) {
        LOG_DEBUG("CNIP_MODE is not ebpf");
        return -1;
    }

    LOG_INFO("eBPF CNIP init: probe=%d, ipv6=%d", cfg->ebpf_enabled, cfg->proxy_ipv6);

    if (boxbpf_probe(cfg->proxy_ipv6) != 0) {
        LOG_WARN("eBPF probe failed");
        return -1;
    }

    if (write_ebpf_config(cfg) != 0) {
        LOG_ERROR("Failed to write eBPF config");
        return -1;
    }

    if (boxbpf_apply(cfg->ebpf_config_path) != 0) {
        LOG_ERROR("Failed to apply eBPF programs");
        return -1;
    }

    LOG_INFO("eBPF CNIP init success (pin: %s)", cfg->ebpf_pin_dir);
    return 0;
}
