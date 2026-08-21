#include "config.h"
#include "atp.h"
#include "atpd_context.h"
#include "logger.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 1024
#define CONFIG_PATH_MAX (PATH_MAX + 256)

static void copy_values(atp_config_t *dst, const atp_config_t *src) {
    dst->core = src->core;
    dst->network = src->network;
    dst->interface = src->interface;
    dst->filter = src->filter;
    dst->service = src->service;
    dst->api = src->api;
}

static int parse_int(const char *value, int *out) {
    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < INT_MIN || parsed > INT_MAX) {
        return ATP_ERR_CONFIG;
    }
    *out = (int)parsed;
    return ATP_OK;
}

static void set_string(char *dst, size_t size, const char *value) {
    snprintf(dst, size, "%s", value);
}

static int parse_value(const char *key, const char *value, atp_config_t *cfg) {
    int *number_target = NULL;

    if (strcmp(key, "ATP_DATA") == 0) {
        set_string(cfg->core.data_dir, sizeof(cfg->core.data_dir), value);
    } else if (strcmp(key, "NETWORK_BACKEND") == 0) {
        set_string(cfg->network.backend, sizeof(cfg->network.backend), value);
    } else if (strcmp(key, "CORE_USER_GROUP") == 0) {
        char pair[128];
        set_string(pair, sizeof(pair), value);
        char *colon = strchr(pair, ':');
        if (!colon || colon == pair || !colon[1]) return ATP_ERR_CONFIG;
        *colon = '\0';
        set_string(cfg->core.core_user, sizeof(cfg->core.core_user), pair);
        set_string(cfg->core.core_group, sizeof(cfg->core.core_group), colon + 1);
    } else if (strcmp(key, "HOTSPOT_INTERFACE") == 0) {
        set_string(cfg->interface.hotspot_iface,
                   sizeof(cfg->interface.hotspot_iface), value);
        cfg->interface.hotspot_iface_explicit = value[0] != '\0';
    } else if (strcmp(key, "USER_CLASH_MODE") == 0) {
        set_string(cfg->filter.user_clash_mode,
                   sizeof(cfg->filter.user_clash_mode), value);
    } else if (strcmp(key, "CLASH_SECRET") == 0) {
        set_string(cfg->filter.clash_secret, sizeof(cfg->filter.clash_secret), value);
    } else if (strcmp(key, "DIRECT_WIFI_SSID") == 0) {
        set_string(cfg->filter.direct_wifi_ssid,
                   sizeof(cfg->filter.direct_wifi_ssid), value);
    } else if (strcmp(key, "API_HOST") == 0) {
        set_string(cfg->api.host, sizeof(cfg->api.host), value);
    } else if (strcmp(key, "SERVICE_ARGS") == 0) {
        set_string(cfg->service.args, sizeof(cfg->service.args), value);
    } else if (strcmp(key, "SERVICE_ENV") == 0) {
        set_string(cfg->service.env, sizeof(cfg->service.env), value);
    } else if (strcmp(key, "API_PORT") == 0) {
        number_target = &cfg->api.port;
    } else if (strcmp(key, "DRY_RUN") == 0) {
        number_target = &cfg->core.dry_run;
    } else if (strcmp(key, "SERVICE_START_TIMEOUT") == 0) {
        number_target = &cfg->service.start_timeout_sec;
    } else if (strcmp(key, "SERVICE_STOP_TIMEOUT") == 0) {
        number_target = &cfg->service.stop_timeout_sec;
    } else if (strcmp(key, "SERVICE_GRACE_PERIOD") == 0) {
        number_target = &cfg->service.grace_period_sec;
    } else if (strcmp(key, "SERVICE_MAX_FAILURES") == 0) {
        number_target = &cfg->service.max_failures;
    } else if (strcmp(key, "SERVICE_CIRCUIT_THRESHOLD") == 0) {
        number_target = &cfg->service.circuit_threshold;
    } else if (strcmp(key, "SERVICE_CIRCUIT_COOLDOWN") == 0) {
        number_target = &cfg->service.circuit_cooldown_sec;
    } else if (strcmp(key, "SERVICE_HEALTH_CHECK_INTERVAL") == 0) {
        number_target = &cfg->service.health_check_interval_ms;
    } else {
        LOG_WARN("Ignoring obsolete or unknown config key: %s", key);
        return ATP_OK;
    }

    return number_target ? parse_int(value, number_target) : ATP_OK;
}

static int valid_mode(const char *mode) {
    return strcmp(mode, "Rule") == 0 || strcmp(mode, "Global") == 0 ||
           strcmp(mode, "Direct") == 0 || strcmp(mode, "Google VPN") == 0;
}

static int config_validate_values(atp_config_t *cfg) {
    if (strcmp(cfg->network.backend, "ebpf") != 0) {
        LOG_ERROR("NETWORK_BACKEND must be ebpf");
        return ATP_ERR_CONFIG;
    }
    if (!cfg->core.data_dir[0] || !cfg->core.core_user[0] ||
        !cfg->core.core_group[0] || !cfg->api.host[0] ||
        cfg->api.port < 1 || cfg->api.port > 65535 ||
        !valid_mode(cfg->filter.user_clash_mode)) {
        LOG_ERROR("Invalid ATP configuration");
        return ATP_ERR_CONFIG;
    }
    if (cfg->service.start_timeout_sec < 1 ||
        cfg->service.stop_timeout_sec < 1 ||
        cfg->service.grace_period_sec < 0 ||
        cfg->service.max_failures < 1 ||
        cfg->service.circuit_threshold < 1 ||
        cfg->service.circuit_cooldown_sec < 1 ||
        cfg->service.health_check_interval_ms < 100) {
        LOG_ERROR("Invalid service timing configuration");
        return ATP_ERR_CONFIG;
    }
    return ATP_OK;
}

void config_set_defaults(atp_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    pthread_mutex_init(&cfg->mutex, NULL);
    set_string(cfg->core.data_dir, sizeof(cfg->core.data_dir), ATP_DEFAULT_DIR);
    set_string(cfg->core.core_user, sizeof(cfg->core.core_user), "root");
    set_string(cfg->core.core_group, sizeof(cfg->core.core_group), "net_admin");
    set_string(cfg->network.backend, sizeof(cfg->network.backend), "ebpf");
    set_string(cfg->interface.hotspot_iface,
               sizeof(cfg->interface.hotspot_iface), "wlan2");
    set_string(cfg->filter.user_clash_mode,
               sizeof(cfg->filter.user_clash_mode), "Rule");
    cfg->service.start_timeout_sec = SERVICE_DEFAULT_START_TIMEOUT_SEC;
    cfg->service.stop_timeout_sec = SERVICE_DEFAULT_STOP_TIMEOUT_SEC;
    cfg->service.grace_period_sec = SERVICE_DEFAULT_GRACE_PERIOD_SEC;
    cfg->service.max_failures = SERVICE_DEFAULT_MAX_FAILURES;
    cfg->service.circuit_threshold = SERVICE_DEFAULT_CIRCUIT_THRESHOLD;
    cfg->service.circuit_cooldown_sec = SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC;
    cfg->service.health_check_interval_ms = SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS;
    cfg->api.port = DEFAULT_API_PORT;
    set_string(cfg->api.host, sizeof(cfg->api.host), DEFAULT_API_HOST);
}

static int config_load_file(const char *path, atp_config_t *cfg) {
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT ? ATP_ERR_NOENT : ATP_ERR_IO;

    int result = ATP_OK;
    char line[CONFIG_LINE_MAX];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *equals = strchr(line, '=');
        if (!equals) {
            LOG_ERROR("Invalid config line %u", line_number);
            result = ATP_ERR_CONFIG;
            break;
        }
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        trim(key);
        trim(value);
        size_t length = strlen(value);
        if (length >= 2 && (value[0] == '\"' || value[0] == '\'') &&
            value[length - 1] == value[0]) {
            value[length - 1] = '\0';
            value++;
        }
        if (parse_value(key, value, cfg) != ATP_OK) {
            LOG_ERROR("Invalid value for %s at line %u", key, line_number);
            result = ATP_ERR_CONFIG;
            break;
        }
    }
    if (ferror(file)) result = ATP_ERR_IO;
    fclose(file);
    return result;
}

int config_load(const char *path, atp_config_t *cfg) {
    if (!path || !cfg) return ATP_ERR_INVAL;
    atp_config_t loaded;
    config_set_defaults(&loaded);
    int result = config_load_file(path, &loaded);
    if (result == ATP_OK) result = config_validate_values(&loaded);
    if (result == ATP_OK) {
        pthread_mutex_lock(&cfg->mutex);
        copy_values(cfg, &loaded);
        pthread_mutex_unlock(&cfg->mutex);
        LOG_INFO("Configuration loaded: %s", path);
    }
    pthread_mutex_destroy(&loaded.mutex);
    return result;
}

static int active_config_path(const atp_config_t *cfg, char *path, size_t size) {
    int written;
    if (g_atpd_ctx.config_path[0]) {
        written = snprintf(path, size, "%s", g_atpd_ctx.config_path);
    } else {
        written = snprintf(path, size, "%s/%s", cfg->core.data_dir, ATP_CONF_FILE);
    }
    return written >= 0 && (size_t)written < size ? ATP_OK : ATP_ERR_INVAL;
}

int config_reload(atp_config_t *cfg) {
    char path[CONFIG_PATH_MAX];
    if (!cfg || active_config_path(cfg, path, sizeof(path)) != ATP_OK) {
        return ATP_ERR_INVAL;
    }
    return config_load(path, cfg);
}

int config_set_mode(atp_config_t *cfg, const char *mode) {
    if (!cfg || !mode || (strcmp(mode, "Rule") != 0 &&
        strcmp(mode, "Global") != 0 && strcmp(mode, "Direct") != 0 &&
        strcmp(mode, "Google VPN") != 0)) return ATP_ERR_INVAL;

    pthread_mutex_lock(&cfg->mutex);
    set_string(cfg->filter.user_clash_mode,
               sizeof(cfg->filter.user_clash_mode), mode);
    pthread_mutex_unlock(&cfg->mutex);

    char path[CONFIG_PATH_MAX];
    if (active_config_path(cfg, path, sizeof(path)) != ATP_OK || !file_exists(path)) {
        return ATP_OK;
    }
    char temporary[CONFIG_PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
        (int)sizeof(temporary)) return ATP_ERR_INVAL;

    FILE *input = fopen(path, "r");
    FILE *output = input ? fopen(temporary, "w") : NULL;
    if (!input || !output) {
        if (input) fclose(input);
        if (output) fclose(output);
        return ATP_ERR_IO;
    }

    int found = 0;
    char line[CONFIG_LINE_MAX];
    while (fgets(line, sizeof(line), input)) {
        char probe[CONFIG_LINE_MAX];
        set_string(probe, sizeof(probe), line);
        trim(probe);
        if (strncmp(probe, "USER_CLASH_MODE=", 16) == 0) {
            fprintf(output, "USER_CLASH_MODE=\"%s\"\n", mode);
            found = 1;
        } else {
            fputs(line, output);
        }
    }
    if (!found) fprintf(output, "USER_CLASH_MODE=\"%s\"\n", mode);

    int result = ATP_OK;
    if (ferror(input) || fflush(output) != 0 || fsync(fileno(output)) != 0) {
        result = ATP_ERR_IO;
    }
    if (fclose(input) != 0 || fclose(output) != 0) result = ATP_ERR_IO;
    if (result == ATP_OK && rename(temporary, path) != 0) result = ATP_ERR_IO;
    if (result != ATP_OK) unlink(temporary);
    return result;
}
