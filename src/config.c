/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration loader & runtime serializer - Pure eBPF Edition
 */

#include "config.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include "config_validator.h"
#include <yyjson.h>
#include <pwd.h>
#include <grp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define SAFE_PATH_MAX (PATH_MAX + 256)
#define DEFAULT_SERVICE_RESTART_DELAY_SEC 2

static void config_sync_from_singbox_json(atp_config_t *cfg);

static int config_parse_int(const char *str, int *out) {
    if (!str || !out) return -1;
    errno = 0;
    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (errno == ERANGE || endptr == str || *endptr != '\0' || val < INT_MIN || val > INT_MAX) return -1;
    *out = (int)val;
    return 0;
}

static void config_apply_run_dir_defaults(atp_config_t *cfg) {
    if (!cfg || !cfg->core.run_dir[0] || strcmp(cfg->core.run_dir, ATP_RUN_DIR) == 0) {
        return;
    }

    /* Keep explicit PID_FILE/LOG_FILE overrides; only move untouched defaults. */
    if (strcmp(cfg->core.pid_file, ATP_PID_FILE) == 0) {
        snprintf(cfg->core.pid_file, sizeof(cfg->core.pid_file),
                 "%s/atpd.pid", cfg->core.run_dir);
    }
    if (strcmp(cfg->core.log_file, ATP_LOG_FILE) == 0) {
        snprintf(cfg->core.log_file, sizeof(cfg->core.log_file),
                 "%s/atp.log", cfg->core.run_dir);
    }
}

static atp_result_t config_prepare(const char *path, atp_config_t *cfg) {
    atp_result_t ret = config_load_file(path, cfg);
    if (ret != ATP_OK) return ret;
    return config_validate_values(cfg) == 0 ? ATP_OK : ATP_ERR_CONFIG;
}

void config_set_defaults(atp_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(atp_config_t));

    cfg->core.ui_emoji_enabled = 1;
    cfg->core.log_timestamp = 1;
    get_app_dir(cfg->core.data_dir, sizeof(cfg->core.data_dir));
    snprintf(cfg->core.run_dir, sizeof(cfg->core.run_dir), "%s", ATP_RUN_DIR);
    snprintf(cfg->core.pid_file, sizeof(cfg->core.pid_file), "%s", ATP_PID_FILE);
    snprintf(cfg->core.log_file, sizeof(cfg->core.log_file), "%s", ATP_LOG_FILE);
    snprintf(cfg->core.core_user, sizeof(cfg->core.core_user), "root");
#ifdef __ANDROID__
    snprintf(cfg->core.core_group, sizeof(cfg->core.core_group), "net_admin");
#else
    struct group *g_chk = getgrnam("net_admin");
    if (g_chk) {
        snprintf(cfg->core.core_group, sizeof(cfg->core.core_group), "net_admin");
    } else {
        snprintf(cfg->core.core_group, sizeof(cfg->core.core_group), "root");
    }
#endif

    cfg->interface.vpn_auto_mode = true;
    snprintf(cfg->interface.vpn_target_mode, sizeof(cfg->interface.vpn_target_mode), "Google VPN");
    snprintf(cfg->interface.vpn_fallback_mode, sizeof(cfg->interface.vpn_fallback_mode), "Rule");

    cfg->service.restart_delay_sec = DEFAULT_SERVICE_RESTART_DELAY_SEC;
    cfg->service.start_timeout_sec = SERVICE_DEFAULT_START_TIMEOUT_SEC;
    cfg->service.stop_timeout_sec = SERVICE_DEFAULT_STOP_TIMEOUT_SEC;
    cfg->service.grace_period_sec = SERVICE_DEFAULT_GRACE_PERIOD_SEC;
    cfg->service.max_failures = SERVICE_DEFAULT_MAX_FAILURES;
    cfg->service.circuit_threshold = SERVICE_DEFAULT_CIRCUIT_THRESHOLD;
    cfg->service.circuit_cooldown_sec = SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC;
    cfg->service.health_check_interval_ms = SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS;
    cfg->service.args[0] = '\0';
    cfg->service.env[0] = '\0';

    cfg->api.port = DEFAULT_API_PORT;
    snprintf(cfg->api.host, sizeof(cfg->api.host), "%s", DEFAULT_API_HOST);
    cfg->api.secret[0] = '\0';

}

static void config_sync_from_singbox_json(atp_config_t *cfg) {
    if (!cfg) return;

    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/config.json", cfg->core.data_dir);
    if (access(conf_path, R_OK) != 0) return;

    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(conf_path, 0, NULL, &err);
    if (!doc) return;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        yyjson_val *api_service = NULL;

        /* 1. Check services array for {"type": "api", "listen": "...", "secret": "..."} */
        yyjson_val *services = yyjson_obj_get(root, "services");
        if (services && yyjson_is_arr(services)) {
            size_t idx, max;
            yyjson_val *svc_item;
            yyjson_arr_foreach(services, idx, max, svc_item) {
                if (yyjson_is_obj(svc_item)) {
                    yyjson_val *type_val = yyjson_obj_get(svc_item, "type");
                    if (type_val && yyjson_is_str(type_val) && strcmp(yyjson_get_str(type_val), "api") == 0) {
                        api_service = svc_item;
                        break;
                    }
                }
            }
        }

        /* 2. Check top-level "api" object */
        if (!api_service) {
            yyjson_val *top_api = yyjson_obj_get(root, "api");
            if (top_api && yyjson_is_obj(top_api)) {
                api_service = top_api;
            }
        }

        /* 3. Check experimental.api */
        if (!api_service) {
            yyjson_val *exp = yyjson_obj_get(root, "experimental");
            if (exp && yyjson_is_obj(exp)) {
                yyjson_val *exp_api = yyjson_obj_get(exp, "api");
                if (exp_api && yyjson_is_obj(exp_api)) {
                    api_service = exp_api;
                }
            }
        }

        if (api_service) {
            yyjson_val *listen_val = yyjson_obj_get(api_service, "listen");
            if (listen_val && yyjson_is_str(listen_val)) {
                const char *listen_str = yyjson_get_str(listen_val);
                if (listen_str && listen_str[0]) {
                    const char *colon = strrchr(listen_str, ':');
                    if (colon) {
                        int port = atoi(colon + 1);
                        if (port > 0 && cfg->api.port == DEFAULT_API_PORT) {
                            cfg->api.port = port;
                        }
                        if (colon != listen_str && strcmp(cfg->api.host, DEFAULT_API_HOST) == 0) {
                            size_t host_len = (size_t)(colon - listen_str);
                            if (host_len < sizeof(cfg->api.host)) {
                                strncpy(cfg->api.host, listen_str, host_len);
                                cfg->api.host[host_len] = '\0';
                            }
                        }
                    } else {
                        if (strcmp(cfg->api.host, DEFAULT_API_HOST) == 0) {
                            snprintf(cfg->api.host, sizeof(cfg->api.host), "%s", listen_str);
                        }
                    }
                }
            }

            yyjson_val *port_val = yyjson_obj_get(api_service, "listen_port");
            if (!port_val) {
                port_val = yyjson_obj_get(api_service, "port");
            }
            if (port_val && yyjson_is_num(port_val)) {
                int p = yyjson_get_int(port_val);
                if (p > 0 && cfg->api.port == DEFAULT_API_PORT) {
                    cfg->api.port = p;
                }
            }

            yyjson_val *secret_val = yyjson_obj_get(api_service, "secret");
            if (secret_val && yyjson_is_str(secret_val)) {
                const char *sec = yyjson_get_str(secret_val);
                if (sec && sec[0] && cfg->api.secret[0] == '\0') {
                    snprintf(cfg->api.secret, sizeof(cfg->api.secret), "%s", sec);
                }
            }
        }

    }

    yyjson_doc_free(doc);
}

static void parse_key_value(const char *k, const char *v, atp_config_t *cfg) {
    int int_val;

    if (config_parse_int(v, &int_val) == 0) {
        if (strcmp(k, "LOG_TIMESTAMP") == 0) cfg->core.log_timestamp = int_val;
        else if (strcmp(k, "RESTART_DELAY") == 0) cfg->service.restart_delay_sec = int_val;
        else if (strcmp(k, "API_PORT") == 0) cfg->api.port = int_val;
        else if (strcmp(k, "UI_EMOJI_ENABLED") == 0) cfg->core.ui_emoji_enabled = int_val;
        else if (strcmp(k, "SERVICE_START_TIMEOUT") == 0) cfg->service.start_timeout_sec = int_val;
        else if (strcmp(k, "SERVICE_STOP_TIMEOUT") == 0) cfg->service.stop_timeout_sec = int_val;
        else if (strcmp(k, "SERVICE_GRACE_PERIOD") == 0) cfg->service.grace_period_sec = int_val;
        else if (strcmp(k, "SERVICE_MAX_FAILURES") == 0) cfg->service.max_failures = int_val;
        else if (strcmp(k, "SERVICE_CIRCUIT_THRESHOLD") == 0) cfg->service.circuit_threshold = int_val;
        else if (strcmp(k, "SERVICE_CIRCUIT_COOLDOWN") == 0) cfg->service.circuit_cooldown_sec = int_val;
        else if (strcmp(k, "SERVICE_HEALTH_CHECK_INTERVAL") == 0) cfg->service.health_check_interval_ms = int_val;
        else if (strcmp(k, "VPN_AUTO_CLASH_MODE") == 0 || strcmp(k, "VPN_AUTO_MODE") == 0) cfg->interface.vpn_auto_mode = (int_val != 0);
    } else {
        if (strcmp(k, "API_SECRET") == 0 || strcmp(k, "CLASH_SECRET") == 0) snprintf(cfg->api.secret, sizeof(cfg->api.secret), "%s", v);
        else if (strcmp(k, "API_HOST") == 0) snprintf(cfg->api.host, sizeof(cfg->api.host), "%s", v);
        else if (strcmp(k, "VPN_TARGET_MODE") == 0 || strcmp(k, "VPN_CLASH_MODE") == 0) snprintf(cfg->interface.vpn_target_mode, sizeof(cfg->interface.vpn_target_mode), "%s", v);
        else if (strcmp(k, "VPN_FALLBACK_MODE") == 0 || strcmp(k, "VPN_DEFAULT_MODE") == 0) snprintf(cfg->interface.vpn_fallback_mode, sizeof(cfg->interface.vpn_fallback_mode), "%s", v);
        else if (strcmp(k, "DATA_DIR") == 0 || strcmp(k, "WORK_DIR") == 0) snprintf(cfg->core.data_dir, sizeof(cfg->core.data_dir), "%s", v);
        else if (strcmp(k, "RUN_DIR") == 0) snprintf(cfg->core.run_dir, sizeof(cfg->core.run_dir), "%s", v);
        else if (strcmp(k, "PID_FILE") == 0) snprintf(cfg->core.pid_file, sizeof(cfg->core.pid_file), "%s", v);
        else if (strcmp(k, "LOG_FILE") == 0) snprintf(cfg->core.log_file, sizeof(cfg->core.log_file), "%s", v);
        else if (strcmp(k, "CORE_USER_GROUP") == 0) {
            char val[256]; snprintf(val, sizeof(val), "%s", v); char *colon = strchr(val, ':');
            if (colon) {
                *colon = '\0';
                snprintf(cfg->core.core_user, sizeof(cfg->core.core_user), "%.63s", val);
                snprintf(cfg->core.core_group, sizeof(cfg->core.core_group), "%.63s", colon + 1);
            }
        }
        else if (strcmp(k, "SERVICE_ARGS") == 0) snprintf(cfg->service.args, sizeof(cfg->service.args), "%s", v);
        else if (strcmp(k, "SERVICE_ENV") == 0) snprintf(cfg->service.env, sizeof(cfg->service.env), "%s", v);
    }
}

atp_result_t config_load_file(const char *path, atp_config_t *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return ATP_ERR_NOENT;
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if ((v[0] == '"' || v[0] == '\'') && strlen(v) >= 2) {
            size_t vlen = strlen(v);
            if (v[vlen-1] == v[0]) {
                v[vlen-1] = '\0';
                memmove(v, v+1, vlen-1);
            }
        }
        parse_key_value(k, v, cfg);
    }
    fclose(fp);
    config_apply_run_dir_defaults(cfg);
    return ATP_OK;
}

atp_result_t config_load(const char *path, atp_config_t *cfg) {
    atp_config_t tmp;
    config_set_defaults(&tmp);

    atp_result_t ret = config_prepare(path, &tmp);
    if (ret != ATP_OK) {
        return ret;
    }

    config_sync_from_singbox_json(&tmp);
    *cfg = tmp;
    LOG_INFO("Configuration loaded: %s", path);
    return ATP_OK;
}

atp_result_t config_reload(atp_config_t *cfg) {
    char cp[SAFE_PATH_MAX];
    if (snprintf(cp, sizeof(cp), "%s/%s", cfg->core.data_dir, ATP_CONF_FILE) >= (int)sizeof(cp)) {
        return ATP_ERR_INVAL;
    }
    if (!file_exists(cp)) {
        return ATP_ERR_NOENT;
    }

    atp_config_t new_config;
    config_set_defaults(&new_config);

    atp_result_t ret = config_prepare(cp, &new_config);
    if (ret != ATP_OK) {
        return ret;
    }
    config_sync_from_singbox_json(&new_config);

    *cfg = new_config;

    LOG_INFO("Configuration reloaded successfully");
    return ATP_OK;
}

atp_result_t validate_interface_name(const char *n) {
    if (!n || !*n) return ATP_ERR_INVAL;
    if (strlen(n) >= IFNAMSIZ) return ATP_ERR_INVAL;
    for (const char *p = n; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr("_+-.", *p)) return ATP_ERR_INVAL;
    }
    return ATP_OK;
}

atp_result_t validate_port(int p) {
    if (p > 0 && p <= 65535) return ATP_OK;
    return ATP_ERR_INVAL;
}

atp_result_t validate_mark(int m) {
    if (m >= 1 && m <= 2147483647) return ATP_OK;
    return ATP_ERR_INVAL;
}
