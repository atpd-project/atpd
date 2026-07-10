#include "boxbpf.h"
#include "bpf_common.h"
#include "logger.h"
#include "utils.h"
#include "app_filter.h"
#include "yyjson.h"
#include "atp_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>

static bool g_ebpf_ready = false;
static char g_pin_dir[256] = "/sys/fs/bpf/box";
static char g_state_dir[256] = "/data/adb/atp/ebpf";
static char g_status_file[512] = "/data/adb/atp/ebpf/ebpf.status";

static sigjmp_buf g_bpf_timeout_env;
static volatile sig_atomic_t g_bpf_timeout = 0;

static void bpf_timeout_handler(int sig) {
    (void)sig;
    g_bpf_timeout = 1;
    siglongjmp(g_bpf_timeout_env, 1);
}

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

static char *read_file_content(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    fseek(fp, 0, SEEK_SET);
    size_t read = fread(buf, 1, size, fp);
    buf[read] = '\0';
    fclose(fp);

    return buf;
}

static int write_ebpf_state_file(const char *state) {
    if (g_state_dir[0] == '\0') {
        return ATP_ERR_INVAL;
    }

    mkdir_recursive(g_state_dir, 0755);

    FILE *fp = fopen(g_status_file, "w");
    if (!fp) {
        LOG_ERROR("Failed to write ebpf.status: %s", g_status_file);
        return ATP_ERR_IO;
    }

    int loaded = 0;
    const char *msg = "Disabled";

    if (strcmp(state, "ready") == 0) {
        loaded = 1;
        msg = "Loaded successfully";
    } else if (strcmp(state, "failed") == 0) {
        loaded = 0;
        msg = "Load failed";
    } else if (strcmp(state, "disabled") == 0) {
        loaded = 0;
        msg = "Disabled by config";
    }

    fprintf(fp, "EBPF_LOADED=%d\n", loaded);
    fprintf(fp, "EBPF_LOAD_TIME=%ld\n", (long)time(NULL));
    fprintf(fp, "EBPF_LOAD_MESSAGE=%s\n", msg);

    fclose(fp);
    LOG_DEBUG("eBPF state written: %s -> %s", state, g_status_file);
    return ATP_OK;
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
        return ATP_ERR_EBPF;
    }

    load_cidr_file(*fd_out, source, ipv6);
    if (pin_replace(*fd_out, pin_path) != 0) {
        return ATP_ERR_EBPF;
    }
    return ATP_OK;
}

static int create_uid_map(const char *source, const char *pin_path,
                          bool required, int *fd_out) {
    *fd_out = create_map(BPF_MAP_TYPE_HASH, sizeof(uint32_t),
                         sizeof(uint8_t), MAX_UIDS, 0);
    if (*fd_out < 0) {
        LOG_ERROR("create UID map failed");
        return ATP_ERR_EBPF;
    }

    int loaded = load_uid_file(*fd_out, source);
    if (required && loaded <= 0) {
        LOG_ERROR("required UID map is empty: %s", source ? source : "-");
        return ATP_ERR_CONFIG;
    }

    if (pin_replace(*fd_out, pin_path) != 0) {
        return ATP_ERR_EBPF;
    }
    return ATP_OK;
}

static int pin_program(const char *section, const char *name,
                       const char *pin_path, const struct fds *fds) {
    int fd = load_program(section, name, fds->cidr4, fds->cidr6,
                          fds->force_uid, fds->app_uid);
    if (fd < 0) return ATP_ERR_EBPF;

    int rc = pin_replace(fd, pin_path);
    close(fd);
    if (rc != 0) return ATP_ERR_EBPF;
    return ATP_OK;
}

static int write_ebpf_config(atp_config_t *cfg) {
    if (!cfg) return ATP_ERR_INVAL;

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

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        LOG_ERROR("Failed to create JSON document");
        return ATP_ERR_NOMEM;
    }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        yyjson_mut_doc_free(doc);
        LOG_ERROR("Failed to create JSON root");
        return ATP_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_bool(doc, root, "ipv6", cfg->proxy_ipv6 ? true : false);
    yyjson_mut_obj_add_str(doc, root, "cidr4", cidr4);
    yyjson_mut_obj_add_str(doc, root, "cidr6", cidr6);
    yyjson_mut_obj_add_str(doc, root, "forceUids", force_uids);
    yyjson_mut_obj_add_str(doc, root, "appUids", app_uids);

    char pin_path[256];
    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_out4", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinCidrOut4", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_out6", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinCidrOut6", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_pre4", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinCidrPre4", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_pre6", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinCidrPre6", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_force_out4", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinForceOut4", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_force_out6", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinForceOut6", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_app_out4", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinAppOut4", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_app_out6", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "pinAppOut6", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr4_lpm", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "mapCidr4", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr6_lpm", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "mapCidr6", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_force_uid_set", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "mapForceUid", pin_path);
    snprintf(pin_path, sizeof(pin_path), "%s/box_app_uid_set", pin_dir);
    yyjson_mut_obj_add_str(doc, root, "mapAppUid", pin_path);

    const char *json_str = yyjson_mut_write(doc, 0, NULL);
    if (!json_str) {
        yyjson_mut_doc_free(doc);
        LOG_ERROR("Failed to serialize JSON");
        return ATP_ERR_EBPF;
    }

    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        free((void *)json_str);
        yyjson_mut_doc_free(doc);
        LOG_ERROR("Failed to create eBPF config: %s", config_path);
        return ATP_ERR_IO;
    }

    fprintf(fp, "%s\n", json_str);
    fclose(fp);

    free((void *)json_str);
    yyjson_mut_doc_free(doc);

    LOG_INFO("eBPF config written: %s", config_path);
    return ATP_OK;
}

static int apply_config(const char *config_path) {
    if (!config_path || access(config_path, R_OK) != 0) {
        LOG_ERROR("eBPF config not found: %s", config_path);
        return ATP_ERR_NOENT;
    }

    char *json_str = read_file_content(config_path);
    if (!json_str) {
        LOG_ERROR("Failed to read config: %s", config_path);
        return ATP_ERR_IO;
    }

    yyjson_doc *doc = yyjson_read(json_str, strlen(json_str), 0);
    if (!doc) {
        LOG_ERROR("Failed to parse JSON config: %s", config_path);
        free(json_str);
        return ATP_ERR_CONFIG;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        free(json_str);
        return ATP_ERR_CONFIG;
    }

    bool ipv6 = false;
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

    yyjson_val *val;
    const char *s;

    val = yyjson_obj_get(root, "ipv6");
    if (val) ipv6 = yyjson_is_true(val);

    s = yyjson_get_str(yyjson_obj_get(root, "cidr4"));
    if (s) strncpy(cidr4_file, s, sizeof(cidr4_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "cidr6"));
    if (s) strncpy(cidr6_file, s, sizeof(cidr6_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "forceUids"));
    if (s) strncpy(force_uid_file, s, sizeof(force_uid_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "appUids"));
    if (s) strncpy(app_uid_file, s, sizeof(app_uid_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrOut4"));
    if (s) strncpy(pin_cidr_out4, s, sizeof(pin_cidr_out4) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrOut6"));
    if (s) strncpy(pin_cidr_out6, s, sizeof(pin_cidr_out6) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrPre4"));
    if (s) strncpy(pin_cidr_pre4, s, sizeof(pin_cidr_pre4) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrPre6"));
    if (s) strncpy(pin_cidr_pre6, s, sizeof(pin_cidr_pre6) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinForceOut4"));
    if (s) strncpy(pin_force_out4, s, sizeof(pin_force_out4) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinForceOut6"));
    if (s) strncpy(pin_force_out6, s, sizeof(pin_force_out6) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinAppOut4"));
    if (s) strncpy(pin_app_out4, s, sizeof(pin_app_out4) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "pinAppOut6"));
    if (s) strncpy(pin_app_out6, s, sizeof(pin_app_out6) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr4"));
    if (s) strncpy(map_cidr4, s, sizeof(map_cidr4) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr6"));
    if (s) strncpy(map_cidr6, s, sizeof(map_cidr6) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapForceUid"));
    if (s) strncpy(map_force_uid, s, sizeof(map_force_uid) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapAppUid"));
    if (s) strncpy(map_app_uid, s, sizeof(map_app_uid) - 1);

    yyjson_doc_free(doc);
    free(json_str);

    if (cidr4_file[0] == '\0') {
        LOG_ERROR("missing cidr4 in config");
        return ATP_ERR_CONFIG;
    }
    if (ipv6 && cidr6_file[0] == '\0') {
        LOG_ERROR("missing cidr6 in config (ipv6 enabled)");
        return ATP_ERR_CONFIG;
    }

    if (pin_cidr_out4[0] == '\0') {
        LOG_ERROR("missing pinCidrOut4 in config");
        return ATP_ERR_CONFIG;
    }
    if (pin_cidr_pre4[0] == '\0') {
        LOG_ERROR("missing pinCidrPre4 in config");
        return ATP_ERR_CONFIG;
    }
    if (pin_force_out4[0] == '\0') {
        LOG_ERROR("missing pinForceOut4 in config");
        return ATP_ERR_CONFIG;
    }
    if (pin_app_out4[0] == '\0') {
        LOG_ERROR("missing pinAppOut4 in config");
        return ATP_ERR_CONFIG;
    }
    if (map_cidr4[0] == '\0') {
        LOG_ERROR("missing mapCidr4 in config");
        return ATP_ERR_CONFIG;
    }
    if (map_force_uid[0] == '\0') {
        LOG_ERROR("missing mapForceUid in config");
        return ATP_ERR_CONFIG;
    }
    if (map_app_uid[0] == '\0') {
        LOG_ERROR("missing mapAppUid in config");
        return ATP_ERR_CONFIG;
    }

    if (ipv6) {
        if (pin_cidr_out6[0] == '\0') {
            LOG_ERROR("missing pinCidrOut6 in config (ipv6 enabled)");
            return ATP_ERR_CONFIG;
        }
        if (pin_cidr_pre6[0] == '\0') {
            LOG_ERROR("missing pinCidrPre6 in config (ipv6 enabled)");
            return ATP_ERR_CONFIG;
        }
        if (pin_force_out6[0] == '\0') {
            LOG_ERROR("missing pinForceOut6 in config (ipv6 enabled)");
            return ATP_ERR_CONFIG;
        }
        if (pin_app_out6[0] == '\0') {
            LOG_ERROR("missing pinAppOut6 in config (ipv6 enabled)");
            return ATP_ERR_CONFIG;
        }
        if (map_cidr6[0] == '\0') {
            LOG_ERROR("missing mapCidr6 in config (ipv6 enabled)");
            return ATP_ERR_CONFIG;
        }
    }

    remove_known_pins();

    int status = ATP_ERR_GENERAL;
    struct fds fds;
    init_fds(&fds);

    bool app_uid_required = uid_file_has_entries(app_uid_file);
    if (create_cidr_map(cidr4_file, map_cidr4, false, &fds.cidr4) != ATP_OK) goto cleanup;
    if (ipv6 && create_cidr_map(cidr6_file, map_cidr6, true, &fds.cidr6) != ATP_OK) goto cleanup;
    if (create_uid_map(force_uid_file, map_force_uid, false, &fds.force_uid) != ATP_OK) goto cleanup;
    if (create_uid_map(app_uid_file, map_app_uid, app_uid_required, &fds.app_uid) != ATP_OK) goto cleanup;

    if (pin_program("socket/cidr4", "cidr4", pin_cidr_out4, &fds) != ATP_OK) goto cleanup;
    if (pin_program("socket/cidr4", "pre4", pin_cidr_pre4, &fds) != ATP_OK) goto cleanup;
    if (pin_program("socket/force4", "force4", pin_force_out4, &fds) != ATP_OK) goto cleanup;
    if (pin_program("socket/appuid", "app4", pin_app_out4, &fds) != ATP_OK) goto cleanup;

    if (ipv6) {
        if (pin_program("socket/cidr6", "cidr6", pin_cidr_out6, &fds) != ATP_OK) goto cleanup;
        if (pin_program("socket/cidr6", "pre6", pin_cidr_pre6, &fds) != ATP_OK) goto cleanup;
        if (pin_program("socket/force6", "force6", pin_force_out6, &fds) != ATP_OK) goto cleanup;
        if (pin_program("socket/appuid", "app6", pin_app_out6, &fds) != ATP_OK) goto cleanup;
    }

    status = ATP_OK;

cleanup:
    if (status != ATP_OK) remove_known_pins();
    close_fds(&fds);

    if (status == ATP_OK) {
        write_ebpf_state_file("ready");
    } else {
        write_ebpf_state_file("failed");
    }

    return status;
}

static int update_config(const char *config_path) {
    if (!config_path || access(config_path, R_OK) != 0) {
        LOG_ERROR("eBPF config not found: %s", config_path);
        write_ebpf_state_file("failed");
        return ATP_ERR_NOENT;
    }

    char *json_str = read_file_content(config_path);
    if (!json_str) {
        LOG_ERROR("Failed to read config: %s", config_path);
        write_ebpf_state_file("failed");
        return ATP_ERR_IO;
    }

    yyjson_doc *doc = yyjson_read(json_str, strlen(json_str), 0);
    if (!doc) {
        LOG_ERROR("Failed to parse JSON config: %s", config_path);
        free(json_str);
        write_ebpf_state_file("failed");
        return ATP_ERR_CONFIG;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        free(json_str);
        write_ebpf_state_file("failed");
        return ATP_ERR_CONFIG;
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

    yyjson_val *val;
    const char *s;

    val = yyjson_obj_get(root, "ipv6");
    if (val) ipv6 = yyjson_is_true(val);

    s = yyjson_get_str(yyjson_obj_get(root, "cidr4"));
    if (s) strncpy(cidr4_file, s, sizeof(cidr4_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "cidr6"));
    if (s) strncpy(cidr6_file, s, sizeof(cidr6_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "forceUids"));
    if (s) strncpy(force_uid_file, s, sizeof(force_uid_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "appUids"));
    if (s) strncpy(app_uid_file, s, sizeof(app_uid_file) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr4"));
    if (s) strncpy(map_cidr4, s, sizeof(map_cidr4) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr6"));
    if (s) strncpy(map_cidr6, s, sizeof(map_cidr6) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapForceUid"));
    if (s) strncpy(map_force_uid, s, sizeof(map_force_uid) - 1);

    s = yyjson_get_str(yyjson_obj_get(root, "mapAppUid"));
    if (s) strncpy(map_app_uid, s, sizeof(map_app_uid) - 1);

    yyjson_doc_free(doc);
    free(json_str);

    if (map_cidr4[0] == '\0') {
        LOG_ERROR("missing mapCidr4 in config");
        return ATP_ERR_CONFIG;
    }
    if (map_force_uid[0] == '\0') {
        LOG_ERROR("missing mapForceUid in config");
        return ATP_ERR_CONFIG;
    }
    if (map_app_uid[0] == '\0') {
        LOG_ERROR("missing mapAppUid in config");
        return ATP_ERR_CONFIG;
    }
    if (cidr4_file[0] == '\0') {
        LOG_ERROR("missing cidr4 in config");
        write_ebpf_state_file("failed");
        return ATP_ERR_CONFIG;
    }

    if (ipv6) {
        if (map_cidr6[0] == '\0') {
            LOG_ERROR("missing mapCidr6 in config (ipv6 enabled)");
            return ATP_ERR_CONFIG;
        }
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
        int ret = apply_config(config_path);
        if (ret == ATP_OK) {
            write_ebpf_state_file("ready");
        } else {
            write_ebpf_state_file("failed");
        }
        return ret;
    }

    int status = ATP_OK;
    if (clear_map(cidr4, sizeof(struct lpm4_key)) < 0) status = ATP_ERR_EBPF;
    load_cidr_file(cidr4, cidr4_file, false);
    if (ipv6) {
        if (clear_map(cidr6, sizeof(struct lpm6_key)) < 0) status = ATP_ERR_EBPF;
        load_cidr_file(cidr6, cidr6_file, true);
    }
    if (clear_map(force_uid, sizeof(uint32_t)) < 0) status = ATP_ERR_EBPF;
    load_uid_file(force_uid, force_uid_file);
    if (clear_map(app_uid, sizeof(uint32_t)) < 0) status = ATP_ERR_EBPF;
    load_uid_file(app_uid, app_uid_file);

    close_fd(cidr4);
    close_fd(cidr6);
    close_fd(force_uid);
    close_fd(app_uid);

    if (status == ATP_OK) {
        write_ebpf_state_file("ready");
    } else {
        write_ebpf_state_file("failed");
    }

    return status;
}

int boxbpf_probe(bool ipv6) {
    struct sigaction sa, old_sa;
    int ret = ATP_OK;

    raise_memlock();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bpf_timeout_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, &old_sa);

    if (sigsetjmp(g_bpf_timeout_env, 1) != 0) {
        LOG_WARN("eBPF probe timeout after 10 seconds");
        sigaction(SIGALRM, &old_sa, NULL);
        return ATP_ERR_TIMEOUT;
    }

    alarm(10);

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

    if (create_cidr_map("/dev/null", map_cidr4, false, &fds.cidr4) != ATP_OK) goto cleanup;
    if (create_cidr_map("/dev/null", map_cidr6, true, &fds.cidr6) != ATP_OK) goto cleanup;
    if (create_uid_map("", map_force_uid, false, &fds.force_uid) != ATP_OK) goto cleanup;
    if (create_uid_map("", map_app_uid, false, &fds.app_uid) != ATP_OK) goto cleanup;

    ok = pin_program("socket/cidr4", "probe4", PROBE_PIN4, &fds) == ATP_OK;
    if (ipv6) {
        ok = pin_program("socket/cidr6", "probe6", PROBE_PIN6, &fds) == ATP_OK && ok;
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

    alarm(0);
    sigaction(SIGALRM, &old_sa, NULL);

    if (ok) {
        ret = ATP_OK;
    } else {
        ret = ATP_ERR_EBPF;
    }

    return ret;
}

int boxbpf_apply(const char *config_path) {
    raise_memlock();
    int ret = apply_config(config_path);
    g_ebpf_ready = (ret == ATP_OK);
    return ret;
}

int boxbpf_update(const char *config_path) {
    raise_memlock();
    return update_config(config_path);
}

int boxbpf_clear(void) {
    remove_known_pins();
    g_ebpf_ready = false;
    write_ebpf_state_file("disabled");
    return ATP_OK;
}

bool boxbpf_is_ready(void) {
    return g_ebpf_ready;
}

const char *boxbpf_pin_dir(void) {
    return g_pin_dir;
}

int boxbpf_init_from_config(atp_config_t *cfg) {
    if (!cfg) return ATP_ERR_INVAL;

    if (cfg->ebpf_state_dir[0] != '\0') {
        strncpy(g_state_dir, cfg->ebpf_state_dir, sizeof(g_state_dir) - 1);
        g_state_dir[sizeof(g_state_dir) - 1] = '\0';
        snprintf(g_status_file, sizeof(g_status_file), "%s/ebpf.status", g_state_dir);
    }

    if (cfg->ebpf_pin_dir[0] != '\0') {
        strncpy(g_pin_dir, cfg->ebpf_pin_dir, sizeof(g_pin_dir) - 1);
        g_pin_dir[sizeof(g_pin_dir) - 1] = '\0';
    }

    if (!cfg->ebpf_enabled) {
        LOG_DEBUG("eBPF disabled by config");
        write_ebpf_state_file("disabled");
        return ATP_ERR_EBPF;
    }

    if (cfg->cnip_mode != 1) {
        LOG_DEBUG("CNIP_MODE is not ebpf");
        write_ebpf_state_file("disabled");
        return ATP_ERR_EBPF;
    }

    LOG_INFO("eBPF CNIP init: probe=%d, ipv6=%d, retry=%d, delay=%d",
             cfg->ebpf_enabled, cfg->proxy_ipv6,
             cfg->ebpf_load_retry, cfg->ebpf_load_delay);

    int retry = cfg->ebpf_load_retry > 0 ? cfg->ebpf_load_retry : 3;
    int delay = cfg->ebpf_load_delay > 0 ? cfg->ebpf_load_delay : 2;
    int probe_ok = 0;

    for (int i = 0; i < retry; i++) {
        if (boxbpf_probe(cfg->proxy_ipv6) == ATP_OK) {
            probe_ok = 1;
            break;
        }
        if (i < retry - 1) {
            LOG_WARN("eBPF probe failed (attempt %d/%d), retrying in %ds...",
                     i + 1, retry, delay);
            sleep(delay);
        } else {
            LOG_WARN("eBPF probe failed (attempt %d/%d)", i + 1, retry);
        }
    }

    if (!probe_ok) {
        LOG_WARN("eBPF probe failed after %d attempts", retry);
        write_ebpf_state_file("failed");
        return ATP_ERR_EBPF;
    }

    if (write_ebpf_config(cfg) != ATP_OK) {
        LOG_ERROR("Failed to write eBPF config");
        write_ebpf_state_file("failed");
        return ATP_ERR_EBPF;
    }

    if (boxbpf_apply(cfg->ebpf_config_path) != ATP_OK) {
        LOG_ERROR("Failed to apply eBPF programs");
        write_ebpf_state_file("failed");
        return ATP_ERR_EBPF;
    }

    write_ebpf_state_file("ready");
    LOG_INFO("eBPF CNIP init success (pin: %s)", cfg->ebpf_pin_dir);
    return ATP_OK;
}

int boxbpf_status(char *state, size_t size, atp_config_t *cfg) {
    struct stat st;
    char pin_path[256];
    const char *pin_dir;
    int ipv6_enabled = 0;
    int force_configured = 0;
    int app_configured = 0;
    int perf_mode = 0;
    int app_proxy = 0;

    (void)cfg;

    if (g_pin_dir[0] != '\0') {
        pin_dir = g_pin_dir;
    } else {
        pin_dir = "/sys/fs/bpf/box";
    }

    if (cfg) {
        ipv6_enabled = cfg->proxy_ipv6;
        force_configured = (cfg->cnip_force_proxy_apps[0] != '\0');
        perf_mode = cfg->performance_mode;
        app_proxy = cfg->app_proxy_enable;
        app_configured = (perf_mode && app_proxy);
    }

    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_out4", pin_dir);
    if (stat(pin_path, &st) != 0) {
        strncpy(state, "uninitialized", size);
        return ATP_ERR_EBPF;
    }

    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_pre4", pin_dir);
    if (stat(pin_path, &st) != 0) {
        strncpy(state, "partial", size);
        return ATP_OK;
    }

    if (ipv6_enabled) {
        snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_out6", pin_dir);
        if (stat(pin_path, &st) != 0) {
            strncpy(state, "partial", size);
            return ATP_OK;
        }
        snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_pre6", pin_dir);
        if (stat(pin_path, &st) != 0) {
            strncpy(state, "partial", size);
            return ATP_OK;
        }
    }

    int force_loaded = 0;
    if (force_configured) {
        snprintf(pin_path, sizeof(pin_path), "%s/box_force_out4", pin_dir);
        if (stat(pin_path, &st) == 0) {
            force_loaded = 1;
        }
        if (ipv6_enabled) {
            snprintf(pin_path, sizeof(pin_path), "%s/box_force_out6", pin_dir);
            if (stat(pin_path, &st) != 0) {
                force_loaded = 0;
            }
        }
    }

    int app_loaded = 0;
    if (app_configured) {
        snprintf(pin_path, sizeof(pin_path), "%s/box_app_out4", pin_dir);
        if (stat(pin_path, &st) == 0) {
            app_loaded = 1;
        }
        if (ipv6_enabled) {
            snprintf(pin_path, sizeof(pin_path), "%s/box_app_out6", pin_dir);
            if (stat(pin_path, &st) != 0) {
                app_loaded = 0;
            }
        }
    }

    if (force_configured && app_configured) {
        if (force_loaded && app_loaded) {
            strncpy(state, "ready", size);
        } else {
            strncpy(state, "partial", size);
        }
    } else if (force_configured) {
        if (force_loaded) {
            strncpy(state, "ready", size);
        } else {
            strncpy(state, "partial", size);
        }
    } else if (app_configured) {
        if (app_loaded) {
            strncpy(state, "ready", size);
        } else {
            strncpy(state, "partial", size);
        }
    } else {
        strncpy(state, "ready_core", size);
    }

    return ATP_OK;
}
