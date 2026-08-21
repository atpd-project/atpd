#include "config_validator.h"
#include "atp.h"
#include "logger.h"

#include <string.h>

static int g_strict_mode;

static const char *const valid_keys[] = {
    "ATP_DATA",
    "NETWORK_BACKEND",
    "CORE_USER_GROUP",
    "HOTSPOT_INTERFACE",
    "USER_CLASH_MODE",
    "CLASH_SECRET",
    "DIRECT_WIFI_SSID",
    "API_HOST",
    "API_PORT",
    "DRY_RUN",
    "SERVICE_START_TIMEOUT",
    "SERVICE_STOP_TIMEOUT",
    "SERVICE_GRACE_PERIOD",
    "SERVICE_MAX_FAILURES",
    "SERVICE_CIRCUIT_THRESHOLD",
    "SERVICE_CIRCUIT_COOLDOWN",
    "SERVICE_HEALTH_CHECK_INTERVAL",
    "SERVICE_ARGS",
    "SERVICE_ENV",
    NULL
};

void config_set_strict_mode(int strict) {
    g_strict_mode = strict != 0;
}

int config_get_strict_mode(void) {
    return g_strict_mode;
}

int config_validate_key(const char *key, atp_config_t *cfg) {
    (void)cfg;
    if (!key || !key[0]) return ATP_ERR_INVAL;
    for (size_t i = 0; valid_keys[i]; i++) {
        if (strcmp(key, valid_keys[i]) == 0) return ATP_OK;
    }
    LOG_WARN("Ignoring obsolete or unknown config key: %s", key);
    return g_strict_mode ? ATP_ERR_CONFIG : ATP_OK;
}

static int valid_mode(const char *mode) {
    return strcmp(mode, "Rule") == 0 || strcmp(mode, "Global") == 0 ||
           strcmp(mode, "Direct") == 0 || strcmp(mode, "Google VPN") == 0;
}

int config_validate_values(atp_config_t *cfg) {
    if (!cfg) return ATP_ERR_INVAL;
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
