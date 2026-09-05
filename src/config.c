/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration loader & runtime serializer
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

typedef struct {
    bool api_port;
    bool api_host;
    bool api_secret;
} config_presence_t;

static atp_result_t config_sync_from_singbox_json(atp_config_t *cfg,
                                                  const config_presence_t *presence);

static int config_parse_int(const char *str, int *out) {
    if (!str || !out) return -1;
    errno = 0;
    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (errno == ERANGE || endptr == str || *endptr != '\0' || val < INT_MIN || val > INT_MAX) return -1;
    *out = (int)val;
    return 0;
}

static int config_parse_bool(const char *str, bool *out) {
    int value;
    if (!out || config_parse_int(str, &value) != 0 || (value != 0 && value != 1)) {
        return -1;
    }
    *out = value != 0;
    return 0;
}

static int config_copy_string(char *dst, size_t dst_size, const char *value) {
    size_t len = strlen(value);
    if (len >= dst_size) return -1;
    memcpy(dst, value, len + 1);
    return 0;
}

static atp_result_t config_parse_error(size_t line_no, const char *key, const char *reason) {
    if (key && key[0]) {
        LOG_ERROR("Invalid configuration at line %zu (key %s): %s", line_no, key, reason);
    } else {
        LOG_ERROR("Invalid configuration at line %zu: %s", line_no, reason);
    }
    return ATP_ERR_CONFIG;
}

static int config_apply_run_dir_defaults(atp_config_t *cfg) {
    if (!cfg || !cfg->core.run_dir[0] || strcmp(cfg->core.run_dir, ATP_RUN_DIR) == 0) {
        return 0;
    }

    /* Keep explicit PID_FILE/LOG_FILE overrides; only move untouched defaults. */
    if (strcmp(cfg->core.pid_file, ATP_PID_FILE) == 0) {
        if (snprintf(cfg->core.pid_file, sizeof(cfg->core.pid_file),
                     "%s/atpd.pid", cfg->core.run_dir) >= (int)sizeof(cfg->core.pid_file)) {
            return -1;
        }
    }
    if (strcmp(cfg->core.log_file, ATP_LOG_FILE) == 0) {
        if (snprintf(cfg->core.log_file, sizeof(cfg->core.log_file),
                     "%s/atp.log", cfg->core.run_dir) >= (int)sizeof(cfg->core.log_file)) {
            return -1;
        }
    }
    return 0;
}

static atp_result_t config_load_file_internal(const char *path, atp_config_t *cfg,
                                              config_presence_t *presence);

static atp_result_t config_prepare(const char *path, atp_config_t *cfg,
                                   config_presence_t *presence) {
    return config_load_file_internal(path, cfg, presence);
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

static atp_result_t config_sync_from_singbox_json(atp_config_t *cfg,
                                                  const config_presence_t *presence) {
    if (!cfg) return ATP_ERR_INVAL;

    char conf_path[PATH_MAX];
    if (snprintf(conf_path, sizeof(conf_path), "%s/config.json", cfg->core.data_dir) >=
        (int)sizeof(conf_path)) {
        return ATP_ERR_CONFIG;
    }
    if (access(conf_path, R_OK) != 0) return ATP_OK;

    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_file(conf_path, 0, NULL, &err);
    if (!doc) return ATP_ERR_CONFIG;

    atp_result_t result = ATP_OK;

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
                        int port;
                        if (config_parse_int(colon + 1, &port) != 0 || validate_port(port) != ATP_OK) {
                            result = ATP_ERR_CONFIG;
                            goto done;
                        }
                        if (!presence || !presence->api_port) {
                            cfg->api.port = port;
                        }
                        if (colon != listen_str && (!presence || !presence->api_host)) {
                            size_t host_len = (size_t)(colon - listen_str);
                            if (host_len >= sizeof(cfg->api.host)) {
                                result = ATP_ERR_CONFIG;
                                goto done;
                            }
                            memcpy(cfg->api.host, listen_str, host_len);
                            cfg->api.host[host_len] = '\0';
                        }
                    } else {
                        if (!presence || !presence->api_host) {
                            if (config_copy_string(cfg->api.host, sizeof(cfg->api.host), listen_str) != 0) {
                                result = ATP_ERR_CONFIG;
                                goto done;
                            }
                        }
                    }
                }
            }

            yyjson_val *port_val = yyjson_obj_get(api_service, "listen_port");
            if (!port_val) {
                port_val = yyjson_obj_get(api_service, "port");
            }
            if (port_val && !yyjson_is_int(port_val)) {
                result = ATP_ERR_CONFIG;
                goto done;
            }
            if (port_val) {
                int p = yyjson_get_int(port_val);
                if (validate_port(p) != ATP_OK) {
                    result = ATP_ERR_CONFIG;
                    goto done;
                }
                if (!presence || !presence->api_port) {
                    cfg->api.port = p;
                }
            }

            yyjson_val *secret_val = yyjson_obj_get(api_service, "secret");
            if (secret_val && !yyjson_is_str(secret_val)) {
                result = ATP_ERR_CONFIG;
                goto done;
            }
            if (secret_val) {
                const char *sec = yyjson_get_str(secret_val);
                if (sec && sec[0] && (!presence || !presence->api_secret)) {
                    if (config_copy_string(cfg->api.secret, sizeof(cfg->api.secret), sec) != 0) {
                        result = ATP_ERR_CONFIG;
                        goto done;
                    }
                }
            }
        }

    }

done:
    yyjson_doc_free(doc);
    return result;
}

static int parse_key_value(const config_key_spec_t *spec, const char *k,
                           const char *v, atp_config_t *cfg) {
    if (!spec || !k || !v || (!spec->allow_empty && !v[0])) return -1;

    if (spec->type == CONFIG_VALUE_INT) {
        int value;
        if (config_parse_int(v, &value) != 0) return -1;
        if (strcmp(k, "RESTART_DELAY") == 0) cfg->service.restart_delay_sec = value;
        else if (strcmp(k, "API_PORT") == 0) cfg->api.port = value;
        else if (strcmp(k, "SERVICE_START_TIMEOUT") == 0) cfg->service.start_timeout_sec = value;
        else if (strcmp(k, "SERVICE_STOP_TIMEOUT") == 0) cfg->service.stop_timeout_sec = value;
        else if (strcmp(k, "SERVICE_GRACE_PERIOD") == 0) cfg->service.grace_period_sec = value;
        else if (strcmp(k, "SERVICE_MAX_FAILURES") == 0) cfg->service.max_failures = value;
        else if (strcmp(k, "SERVICE_CIRCUIT_THRESHOLD") == 0) cfg->service.circuit_threshold = value;
        else if (strcmp(k, "SERVICE_CIRCUIT_COOLDOWN") == 0) cfg->service.circuit_cooldown_sec = value;
        else if (strcmp(k, "SERVICE_HEALTH_CHECK_INTERVAL") == 0) cfg->service.health_check_interval_ms = value;
        else return -1;
        return 0;
    }

    if (spec->type == CONFIG_VALUE_BOOL) {
        bool value;
        if (config_parse_bool(v, &value) != 0) return -1;
        if (strcmp(k, "LOG_TIMESTAMP") == 0) cfg->core.log_timestamp = value;
        else if (strcmp(k, "UI_EMOJI_ENABLED") == 0) cfg->core.ui_emoji_enabled = value;
        else if (strcmp(k, "VPN_AUTO_CLASH_MODE") == 0 || strcmp(k, "VPN_AUTO_MODE") == 0) {
            cfg->interface.vpn_auto_mode = value;
        } else return -1;
        return 0;
    }

    if (strcmp(k, "API_SECRET") == 0 || strcmp(k, "CLASH_SECRET") == 0) {
        return config_copy_string(cfg->api.secret, sizeof(cfg->api.secret), v);
    }
    if (strcmp(k, "API_HOST") == 0) {
        return config_copy_string(cfg->api.host, sizeof(cfg->api.host), v);
    }
    if (strcmp(k, "VPN_TARGET_MODE") == 0 || strcmp(k, "VPN_CLASH_MODE") == 0) {
        return config_copy_string(cfg->interface.vpn_target_mode,
                                  sizeof(cfg->interface.vpn_target_mode), v);
    }
    if (strcmp(k, "VPN_FALLBACK_MODE") == 0 || strcmp(k, "VPN_DEFAULT_MODE") == 0) {
        return config_copy_string(cfg->interface.vpn_fallback_mode,
                                  sizeof(cfg->interface.vpn_fallback_mode), v);
    }
    if (strcmp(k, "DATA_DIR") == 0 || strcmp(k, "WORK_DIR") == 0) {
        return config_copy_string(cfg->core.data_dir, sizeof(cfg->core.data_dir), v);
    }
    if (strcmp(k, "RUN_DIR") == 0) {
        return config_copy_string(cfg->core.run_dir, sizeof(cfg->core.run_dir), v);
    }
    if (strcmp(k, "PID_FILE") == 0) {
        return config_copy_string(cfg->core.pid_file, sizeof(cfg->core.pid_file), v);
    }
    if (strcmp(k, "LOG_FILE") == 0) {
        return config_copy_string(cfg->core.log_file, sizeof(cfg->core.log_file), v);
    }
    if (strcmp(k, "CORE_USER_GROUP") == 0) {
        const char *colon = strchr(v, ':');
        size_t user_len = colon ? (size_t)(colon - v) : 0;
        size_t group_len = colon ? strlen(colon + 1) : 0;
        if (!colon || !user_len || !group_len || strchr(colon + 1, ':') ||
            user_len >= sizeof(cfg->core.core_user) ||
            group_len >= sizeof(cfg->core.core_group)) {
            return -1;
        }
        memcpy(cfg->core.core_user, v, user_len);
        cfg->core.core_user[user_len] = '\0';
        memcpy(cfg->core.core_group, colon + 1, group_len + 1);
        return 0;
    }
    if (strcmp(k, "SERVICE_ARGS") == 0) {
        return config_copy_string(cfg->service.args, sizeof(cfg->service.args), v);
    }
    if (strcmp(k, "SERVICE_ENV") == 0) {
        return config_copy_string(cfg->service.env, sizeof(cfg->service.env), v);
    }
    return -1;
}

static atp_result_t config_load_file_internal(const char *path, atp_config_t *cfg,
                                              config_presence_t *presence) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return ATP_ERR_NOENT;
    }
    char line[1024];
    const char *seen_keys[64];
    size_t seen_count = 0;
    size_t line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(fp)) {
            while (fgets(line, sizeof(line), fp) && !strchr(line, '\n')) { }
            fclose(fp);
            return config_parse_error(line_no, NULL, "line exceeds 1023 bytes");
        }
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) {
            fclose(fp);
            return config_parse_error(line_no, NULL, "expected KEY=VALUE");
        }
        *eq = '\0';
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!k[0]) {
            fclose(fp);
            return config_parse_error(line_no, NULL, "empty key");
        }
        if ((v[0] == '"' || v[0] == '\'') && strlen(v) >= 2) {
            size_t vlen = strlen(v);
            if (v[vlen-1] == v[0]) {
                v[vlen-1] = '\0';
                memmove(v, v+1, vlen-1);
            } else {
                fclose(fp);
                return config_parse_error(line_no, k, "unterminated quote");
            }
        } else if (v[0] == '"' || v[0] == '\'') {
            fclose(fp);
            return config_parse_error(line_no, k, "unterminated quote");
        }
        const config_key_spec_t *spec = config_schema_find(k);
        if (!spec) {
            fclose(fp);
            return config_parse_error(line_no, k, "unknown key");
        }
        if (presence) {
            if (strcmp(spec->canonical_name, "API_PORT") == 0) presence->api_port = true;
            if (strcmp(spec->canonical_name, "API_HOST") == 0) presence->api_host = true;
            if (strcmp(spec->canonical_name, "API_SECRET") == 0) presence->api_secret = true;
        }
        if (spec->deprecated) {
            LOG_WARN("Deprecated configuration key %s; use %s", k, spec->canonical_name);
        }
        for (size_t i = 0; i < seen_count; i++) {
            if (strcmp(seen_keys[i], spec->canonical_name) == 0) {
                fclose(fp);
                return config_parse_error(line_no, k, "duplicate key");
            }
        }
        if (seen_count < sizeof(seen_keys) / sizeof(seen_keys[0])) {
            seen_keys[seen_count++] = spec->canonical_name;
        }
        if (parse_key_value(spec, k, v, cfg) != 0) {
            fclose(fp);
            return config_parse_error(line_no, k, "invalid value");
        }
    }
    if (ferror(fp)) {
        fclose(fp);
        return ATP_ERR_IO;
    }
    if (fclose(fp) != 0) return ATP_ERR_IO;
    if (config_apply_run_dir_defaults(cfg) != 0) {
        LOG_ERROR("Invalid configuration: RUN_DIR leaves no room for default file paths");
        return ATP_ERR_CONFIG;
    }
    return ATP_OK;
}

atp_result_t config_load_file(const char *path, atp_config_t *cfg) {
    return config_load_file_internal(path, cfg, NULL);
}

atp_result_t config_load(const char *path, atp_config_t *cfg) {
    if (!path || !cfg) return ATP_ERR_INVAL;
    atp_config_t tmp;
    config_presence_t presence = {0};
    config_set_defaults(&tmp);

    atp_result_t ret = config_prepare(path, &tmp, &presence);
    if (ret != ATP_OK) {
        return ret;
    }

    ret = config_sync_from_singbox_json(&tmp, &presence);
    if (ret != ATP_OK || config_validate_values(&tmp) != 0) {
        return ATP_ERR_CONFIG;
    }
    *cfg = tmp;
    LOG_INFO("Configuration loaded: %s", path);
    return ATP_OK;
}

atp_result_t config_prepare_reload(const char *source_path, atp_config_t *candidate) {
    if (!source_path || !source_path[0] || !candidate) return ATP_ERR_INVAL;
    if (!file_exists(source_path)) {
        return ATP_ERR_NOENT;
    }

    atp_config_t new_config;
    config_presence_t presence = {0};
    config_set_defaults(&new_config);

    atp_result_t ret = config_prepare(source_path, &new_config, &presence);
    if (ret != ATP_OK) {
        return ret;
    }
    ret = config_sync_from_singbox_json(&new_config, &presence);
    if (ret != ATP_OK || config_validate_values(&new_config) != 0) {
        return ATP_ERR_CONFIG;
    }

    *candidate = new_config;
    return ATP_OK;
}

static void config_record_changed_field(char *fields, size_t fields_size,
                                        const char *name) {
    if (!fields || fields_size == 0 || !name) return;

    size_t used = strlen(fields);
    if (used >= fields_size - 1) return;
    snprintf(fields + used, fields_size - used, "%s%s", used ? ", " : "", name);
}

config_reload_changes_t config_classify_reload(const atp_config_t *current,
                                               const atp_config_t *candidate,
                                               char *changed_fields,
                                               size_t changed_fields_size) {
    if (changed_fields && changed_fields_size > 0) changed_fields[0] = '\0';
    if (!current || !candidate) return CONFIG_RELOAD_CHANGE_REQUIRES_RESTART;

    config_reload_changes_t changes = CONFIG_RELOAD_CHANGE_NONE;
#define RESTART_IF_CHANGED(condition, name) do { \
        if (condition) { \
            changes |= CONFIG_RELOAD_CHANGE_REQUIRES_RESTART; \
            config_record_changed_field(changed_fields, changed_fields_size, name); \
        } \
    } while (0)
#define HOT_IF_CHANGED(condition) do { \
        if (condition) changes |= CONFIG_RELOAD_CHANGE_HOT; \
    } while (0)
#define STATIC_IF_CHANGED(condition) do { \
        if (condition) changes |= CONFIG_RELOAD_CHANGE_STATIC; \
    } while (0)

    HOT_IF_CHANGED(current->core.ui_emoji_enabled != candidate->core.ui_emoji_enabled);
    HOT_IF_CHANGED(current->service.start_timeout_sec !=
                   candidate->service.start_timeout_sec);
    HOT_IF_CHANGED(current->service.stop_timeout_sec !=
                   candidate->service.stop_timeout_sec);
    HOT_IF_CHANGED(current->service.max_failures != candidate->service.max_failures);
    HOT_IF_CHANGED(current->service.health_check_interval_ms !=
                   candidate->service.health_check_interval_ms);

    STATIC_IF_CHANGED(current->core.log_timestamp != candidate->core.log_timestamp);
    STATIC_IF_CHANGED(current->service.restart_delay_sec !=
                      candidate->service.restart_delay_sec);
    STATIC_IF_CHANGED(current->service.grace_period_sec !=
                      candidate->service.grace_period_sec);

    RESTART_IF_CHANGED(strcmp(current->core.data_dir, candidate->core.data_dir) != 0,
                       "DATA_DIR");
    RESTART_IF_CHANGED(strcmp(current->core.run_dir, candidate->core.run_dir) != 0,
                       "RUN_DIR");
    RESTART_IF_CHANGED(strcmp(current->core.pid_file, candidate->core.pid_file) != 0,
                       "PID_FILE");
    RESTART_IF_CHANGED(strcmp(current->core.log_file, candidate->core.log_file) != 0,
                       "LOG_FILE");
    RESTART_IF_CHANGED(strcmp(current->core.core_user, candidate->core.core_user) != 0 ||
                       strcmp(current->core.core_group, candidate->core.core_group) != 0,
                       "CORE_USER_GROUP");
    RESTART_IF_CHANGED(strcmp(current->service.args, candidate->service.args) != 0,
                       "SERVICE_ARGS");
    RESTART_IF_CHANGED(strcmp(current->service.env, candidate->service.env) != 0,
                       "SERVICE_ENV");
    RESTART_IF_CHANGED(strcmp(current->api.host, candidate->api.host) != 0,
                       "API_HOST");
    RESTART_IF_CHANGED(current->api.port != candidate->api.port, "API_PORT");
    RESTART_IF_CHANGED(strcmp(current->api.secret, candidate->api.secret) != 0,
                       "API_SECRET");
    RESTART_IF_CHANGED(current->interface.vpn_auto_mode != candidate->interface.vpn_auto_mode,
                       "VPN_AUTO_MODE");
    RESTART_IF_CHANGED(strcmp(current->interface.vpn_target_mode,
                              candidate->interface.vpn_target_mode) != 0,
                       "VPN_TARGET_MODE");
    RESTART_IF_CHANGED(strcmp(current->interface.vpn_fallback_mode,
                              candidate->interface.vpn_fallback_mode) != 0,
                       "VPN_FALLBACK_MODE");
    RESTART_IF_CHANGED(current->service.circuit_threshold !=
                       candidate->service.circuit_threshold,
                       "SERVICE_CIRCUIT_THRESHOLD");
    RESTART_IF_CHANGED(current->service.circuit_cooldown_sec !=
                       candidate->service.circuit_cooldown_sec,
                       "SERVICE_CIRCUIT_COOLDOWN");

#undef RESTART_IF_CHANGED
#undef HOT_IF_CHANGED
#undef STATIC_IF_CHANGED
    return changes;
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
