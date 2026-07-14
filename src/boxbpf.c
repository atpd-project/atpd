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
#include <stdatomic.h>

static atomic_bool g_ebpf_ready = false;
static char g_pin_dir[256] = "/sys/fs/bpf/box";
static char g_state_dir[256] = "/data/adb/atp/ebpf";
static char g_status_file[512] = "/data/adb/atp/ebpf/ebpf.status";

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

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

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
    if (read != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);

    return buf;
}

static int write_ebpf_state_file(const char *state) {
    if (g_state_dir[0] == '\0') {
        return ATP_ERR_INVAL;
    }

    if (mkdir_recursive(g_state_dir, 0755) != 0) {
        LOG_ERROR("Failed to create state dir: %s", g_state_dir);
        return ATP_ERR_IO;
    }

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

    int loaded = load_cidr_file(*fd_out, source, ipv6);
    if (loaded < 0) {
        LOG_ERROR("Failed to load CIDR file: %s", source);
        close_fd(*fd_out);
        *fd_out = -1;
        return ATP_ERR_CONFIG;
    }

    if (pin_replace(*fd_out, pin_path) != 0) {
        LOG_ERROR("Failed to pin CIDR map: %s", pin_path);
        close_fd(*fd_out);
        *fd_out = -1;
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
    if (loaded < 0) {
        LOG_ERROR("Failed to load UID file: %s", source ? source : "-");
        close_fd(*fd_out);
        *fd_out = -1;
        return ATP_ERR_CONFIG;
    }

    if (required && loaded <= 0) {
        LOG_ERROR("required UID map is empty: %s", source ? source : "-");
        close_fd(*fd_out);
        *fd_out = -1;
        return ATP_ERR_CONFIG;
    }

    if (pin_replace(*fd_out, pin_path) != 0) {
        LOG_ERROR("Failed to pin UID map: %s", pin_path);
        close_fd(*fd_out);
        *fd_out = -1;
        return ATP_ERR_EBPF;
    }
    return ATP_OK;
}

static int pin_program(const char *section, const char *name,
                       const char *pin_path, const struct fds *fds) {
    int fd = load_program(section, name, fds->cidr4, fds->cidr6,
                          fds->force_uid, fds->app_uid);
    if (fd < 0) {
        LOG_ERROR("load_program(%s) failed", section);
        return ATP_ERR_EBPF;
    }

    int rc = pin_replace(fd, pin_path);
    close(fd);
    if (rc != 0) {
        LOG_ERROR("pin_replace(%s) failed", pin_path);
        return ATP_ERR_EBPF;
    }
    return ATP_OK;
}

int write_ebpf_config(atp_config_t *cfg) {
    if (!cfg) return ATP_ERR_INVAL;

    char *state_dir = cfg->ebpf.state_dir;
    char *pin_dir = cfg->ebpf.pin_dir;
    char config_path[512];
    char empty_v4[512], empty_v6[512], force_uids[512], app_uids[512];

    snprintf(config_path, sizeof(config_path), "%s/config.json", state_dir);
    snprintf(empty_v4, sizeof(empty_v4), "%s/empty-v4.txt", state_dir);
    snprintf(empty_v6, sizeof(empty_v6), "%s/empty-v6.txt", state_dir);
    snprintf(force_uids, sizeof(force_uids), "%s/force-uids.txt", state_dir);
    snprintf(app_uids, sizeof(app_uids), "%s/app-uids.txt", state_dir);

    if (mkdir_recursive(state_dir, 0755) != 0) {
        LOG_ERROR("Failed to create state dir: %s", state_dir);
        return ATP_ERR_IO;
    }
    if (mkdir_recursive(pin_dir, 0755) != 0) {
        LOG_ERROR("Failed to create pin dir: %s", pin_dir);
        return ATP_ERR_IO;
    }

    FILE *f = fopen(empty_v4, "w");
    if (f) fclose(f);
    f = fopen(empty_v6, "w");
    if (f) fclose(f);

    FILE *fu = fopen(force_uids, "w");
    if (fu) {
        if (cfg->filter.cnip_force_proxy_apps[0] != '\0') {
            char *copy = strdup(cfg->filter.cnip_force_proxy_apps);
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
        if (cfg->core.performance_mode && cfg->filter.app_proxy_enable) {
            const char *pkg_list = NULL;
            if (strcmp(cfg->filter.app_proxy_mode, "blacklist") == 0) {
                pkg_list = cfg->filter.bypass_apps_list;
            } else {
                pkg_list = cfg->filter.proxy_apps_list;
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

    if (cfg->filter.bypass_cn_ip) {
        char cn_path[512];
        snprintf(cn_path, sizeof(cn_path), "%s/%s", cfg->core.data_dir, cfg->filter.cn_ip_file);
        if (file_exists(cn_path)) {
            snprintf(cidr4, sizeof(cidr4), "%s", cn_path);
        }
        if (cfg->network.proxy_ipv6) {
            snprintf(cn_path, sizeof(cn_path), "%s/%s", cfg->core.data_dir, cfg->filter.cn_ipv6_file);
            if (file_exists(cn_path)) {
                snprintf(cidr6, sizeof(cidr6), "%s", cn_path);
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

    yyjson_mut_obj_add_bool(doc, root, "ipv6", cfg->network.proxy_ipv6 ? true : false);
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
    if (s) snprintf(cidr4_file, sizeof(cidr4_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "cidr6"));
    if (s) snprintf(cidr6_file, sizeof(cidr6_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "forceUids"));
    if (s) snprintf(force_uid_file, sizeof(force_uid_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "appUids"));
    if (s) snprintf(app_uid_file, sizeof(app_uid_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrOut4"));
    if (s) snprintf(pin_cidr_out4, sizeof(pin_cidr_out4), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrOut6"));
    if (s) snprintf(pin_cidr_out6, sizeof(pin_cidr_out6), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrPre4"));
    if (s) snprintf(pin_cidr_pre4, sizeof(pin_cidr_pre4), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinCidrPre6"));
    if (s) snprintf(pin_cidr_pre6, sizeof(pin_cidr_pre6), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinForceOut4"));
    if (s) snprintf(pin_force_out4, sizeof(pin_force_out4), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinForceOut6"));
    if (s) snprintf(pin_force_out6, sizeof(pin_force_out6), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinAppOut4"));
    if (s) snprintf(pin_app_out4, sizeof(pin_app_out4), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "pinAppOut6"));
    if (s) snprintf(pin_app_out6, sizeof(pin_app_out6), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr4"));
    if (s) snprintf(map_cidr4, sizeof(map_cidr4), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr6"));
    if (s) snprintf(map_cidr6, sizeof(map_cidr6), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapForceUid"));
    if (s) snprintf(map_force_uid, sizeof(map_force_uid), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapAppUid"));
    if (s) snprintf(map_app_uid, sizeof(map_app_uid), "%s", s);

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
    if (status != ATP_OK) {
        remove_known_pins();
    }
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
    if (s) snprintf(cidr4_file, sizeof(cidr4_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "cidr6"));
    if (s) snprintf(cidr6_file, sizeof(cidr6_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "forceUids"));
    if (s) snprintf(force_uid_file, sizeof(force_uid_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "appUids"));
    if (s) snprintf(app_uid_file, sizeof(app_uid_file), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr4"));
    if (s) snprintf(map_cidr4, sizeof(map_cidr4), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapCidr6"));
    if (s) snprintf(map_cidr6, sizeof(map_cidr6), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapForceUid"));
    if (s) snprintf(map_force_uid, sizeof(map_force_uid), "%s", s);

    s = yyjson_get_str(yyjson_obj_get(root, "mapAppUid"));
    if (s) snprintf(map_app_uid, sizeof(map_app_uid), "%s", s);

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

    if (clear_map(cidr4, sizeof(struct lpm4_key)) < 0) {
        status = ATP_ERR_EBPF;
    }
    if (load_cidr_file(cidr4, cidr4_file, false) < 0) {
        LOG_ERROR("Failed to reload CIDR4 map");
        status = ATP_ERR_CONFIG;
    }

    if (ipv6) {
        if (clear_map(cidr6, sizeof(struct lpm6_key)) < 0) {
            status = ATP_ERR_EBPF;
        }
        if (load_cidr_file(cidr6, cidr6_file, true) < 0) {
            LOG_ERROR("Failed to reload CIDR6 map");
            status = ATP_ERR_CONFIG;
        }
    }

    if (clear_map(force_uid, sizeof(uint32_t)) < 0) {
        status = ATP_ERR_EBPF;
    }
    if (load_uid_file(force_uid, force_uid_file) < 0) {
        LOG_ERROR("Failed to reload force UID map");
        status = ATP_ERR_CONFIG;
    }

    if (clear_map(app_uid, sizeof(uint32_t)) < 0) {
        status = ATP_ERR_EBPF;
    }
    if (load_uid_file(app_uid, app_uid_file) < 0) {
        LOG_ERROR("Failed to reload app UID map");
        status = ATP_ERR_CONFIG;
    }

    close_fd(cidr4);
    close_fd(cidr6);
    close_fd(force_uid);
    close_fd(app_uid);

    atomic_store(&g_ebpf_ready, status == ATP_OK);

    if (status == ATP_OK) {
        write_ebpf_state_file("ready");
    } else {
        write_ebpf_state_file("failed");
    }

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
    if (unlink(PROBE_PIN4) != 0 && errno != ENOENT) {
        LOG_WARN("Failed to unlink %s: %s", PROBE_PIN4, strerror(errno));
    }
    if (unlink(PROBE_PIN6) != 0 && errno != ENOENT) {
        LOG_WARN("Failed to unlink %s: %s", PROBE_PIN6, strerror(errno));
    }
    if (unlink(map_cidr4) != 0 && errno != ENOENT) {
        LOG_WARN("Failed to unlink %s: %s", map_cidr4, strerror(errno));
    }
    if (unlink(map_cidr6) != 0 && errno != ENOENT) {
        LOG_WARN("Failed to unlink %s: %s", map_cidr6, strerror(errno));
    }
    if (unlink(map_force_uid) != 0 && errno != ENOENT) {
        LOG_WARN("Failed to unlink %s: %s", map_force_uid, strerror(errno));
    }
    if (unlink(map_app_uid) != 0 && errno != ENOENT) {
        LOG_WARN("Failed to unlink %s: %s", map_app_uid, strerror(errno));
    }
    if (rmdir(PIN_DIR) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        LOG_WARN("Failed to rmdir %s: %s", PIN_DIR, strerror(errno));
    }

    atomic_store(&g_ebpf_ready, ok);

    if (ok) {
        return ATP_OK;
    } else {
        return ATP_ERR_EBPF;
    }
}

int boxbpf_apply(const char *config_path) {
    raise_memlock();
    int ret = apply_config(config_path);
    atomic_store(&g_ebpf_ready, (ret == ATP_OK));
    return ret;
}

int boxbpf_update(const char *config_path) {
    raise_memlock();
    return update_config(config_path);
}

int boxbpf_clear(void) {
    remove_known_pins();
    atomic_store(&g_ebpf_ready, false);
    write_ebpf_state_file("disabled");
    return ATP_OK;
}

bool boxbpf_is_ready(void) {
    return atomic_load(&g_ebpf_ready);
}

const char *boxbpf_pin_dir(void) {
    return g_pin_dir;
}

int boxbpf_init_from_config(atp_config_t *cfg) {
    if (!cfg) return ATP_ERR_INVAL;

    if (cfg->ebpf.state_dir[0] != '\0') {
        snprintf(g_state_dir, sizeof(g_state_dir), "%s", cfg->ebpf.state_dir);
        snprintf(g_status_file, sizeof(g_status_file), "%s/ebpf.status", g_state_dir);
    }

    if (cfg->ebpf.pin_dir[0] != '\0') {
        snprintf(g_pin_dir, sizeof(g_pin_dir), "%s", cfg->ebpf.pin_dir);
    }

    if (!cfg->ebpf.enabled) {
        LOG_DEBUG("eBPF disabled by config");
        write_ebpf_state_file("disabled");
        return ATP_ERR_EBPF;
    }

    if (cfg->filter.cnip_mode != 1) {
        LOG_DEBUG("CNIP_MODE is not ebpf");
        write_ebpf_state_file("disabled");
        return ATP_ERR_EBPF;
    }

    LOG_INFO("eBPF CNIP init: probe=%d, ipv6=%d, retry=%d, delay=%d",
             cfg->ebpf.enabled, cfg->network.proxy_ipv6,
             cfg->ebpf.load_retry, cfg->ebpf.load_delay);

    int retry = cfg->ebpf.load_retry > 0 ? cfg->ebpf.load_retry : 3;
    int delay = cfg->ebpf.load_delay > 0 ? cfg->ebpf.load_delay : 2;
    int probe_ok = 0;

    for (int i = 0; i < retry; i++) {
        if (boxbpf_probe(cfg->network.proxy_ipv6) == ATP_OK) {
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
        LOG_ERROR("eBPF probe failed after %d attempts", retry);
        write_ebpf_state_file("failed");
        return ATP_ERR_EBPF;
    }

    if (write_ebpf_config(cfg) != ATP_OK) {
        LOG_ERROR("Failed to write eBPF config");
        write_ebpf_state_file("failed");
        return ATP_ERR_EBPF;
    }

    if (boxbpf_apply(cfg->ebpf.config_path) != ATP_OK) {
        LOG_ERROR("Failed to apply eBPF programs");
        write_ebpf_state_file("failed");
        return ATP_ERR_EBPF;
    }

    write_ebpf_state_file("ready");
    cfg->ebpf.ready = 1;
    LOG_INFO("eBPF CNIP init success (pin: %s)", cfg->ebpf.pin_dir);
    return ATP_OK;
}

int boxbpf_reload_from_config(atp_config_t *cfg) {
    if (!cfg) return ATP_ERR_INVAL;

    if (!cfg->ebpf.enabled) {
        LOG_DEBUG("eBPF disabled by config, skipping reload");
        write_ebpf_state_file("disabled");
        return ATP_ERR_EBPF;
    }

    if (cfg->filter.cnip_mode != 1) {
        LOG_DEBUG("CNIP_MODE is not ebpf, skipping reload");
        write_ebpf_state_file("disabled");
        return ATP_ERR_EBPF;
    }

    if (cfg->ebpf.state_dir[0] != '\0') {
        snprintf(g_state_dir, sizeof(g_state_dir), "%s", cfg->ebpf.state_dir);
        snprintf(g_status_file, sizeof(g_status_file), "%s/ebpf.status", g_state_dir);
    }

    if (cfg->ebpf.pin_dir[0] != '\0') {
        snprintf(g_pin_dir, sizeof(g_pin_dir), "%s", cfg->ebpf.pin_dir);
    }

    if (cfg->ebpf.ready) {
        int ret = boxbpf_update(cfg->ebpf.config_path);
        if (ret == ATP_OK) {
            LOG_INFO("eBPF CNIP maps updated successfully");
            return ATP_OK;
        } else {
            LOG_ERROR("eBPF CNIP update failed, reloading from scratch");
            boxbpf_clear();
            cfg->ebpf.ready = 0;
            return boxbpf_init_from_config(cfg);
        }
    } else {
        return boxbpf_init_from_config(cfg);
    }
}

int boxbpf_status(char *state, size_t size, atp_config_t *cfg) {
    if (!state || size == 0) {
        return ATP_ERR_INVAL;
    }

    struct stat st;
    char pin_path[256];
    const char *pin_dir;
    int ipv6_enabled = 0;
    int force_configured = 0;
    int app_configured = 0;
    int perf_mode = 0;
    int app_proxy = 0;

    if (cfg && cfg->ebpf.pin_dir[0] != '\0') {
        pin_dir = cfg->ebpf.pin_dir;
    } else if (g_pin_dir[0] != '\0') {
        pin_dir = g_pin_dir;
    } else {
        pin_dir = "/sys/fs/bpf/box";
    }

    if (cfg) {
        ipv6_enabled = cfg->network.proxy_ipv6;
        force_configured = (cfg->filter.cnip_force_proxy_apps[0] != '\0');
        perf_mode = cfg->core.performance_mode;
        app_proxy = cfg->filter.app_proxy_enable;
        app_configured = (perf_mode && app_proxy);
    }

    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_out4", pin_dir);
    if (stat(pin_path, &st) != 0) {
        snprintf(state, size, "%s", "uninitialized");
        return ATP_ERR_EBPF;
    }

    snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_pre4", pin_dir);
    if (stat(pin_path, &st) != 0) {
        snprintf(state, size, "%s", "partial");
        return ATP_OK;
    }

    if (ipv6_enabled) {
        snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_out6", pin_dir);
        if (stat(pin_path, &st) != 0) {
            snprintf(state, size, "%s", "partial");
            return ATP_OK;
        }
        snprintf(pin_path, sizeof(pin_path), "%s/box_cidr_pre6", pin_dir);
        if (stat(pin_path, &st) != 0) {
            snprintf(state, size, "%s", "partial");
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
            snprintf(state, size, "%s", "ready");
        } else {
            snprintf(state, size, "%s", "partial");
        }
    } else if (force_configured) {
        if (force_loaded) {
            snprintf(state, size, "%s", "ready");
        } else {
            snprintf(state, size, "%s", "partial");
        }
    } else if (app_configured) {
        if (app_loaded) {
            snprintf(state, size, "%s", "ready");
        } else {
            snprintf(state, size, "%s", "partial");
        }
    } else {
        snprintf(state, size, "%s", "ready_core");
    }

    return ATP_OK;
}
