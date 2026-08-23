/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration validator implementation - Pure eBPF Edition
 */

#include "config_validator.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <net/if.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>

#define MAX_KEY_LEN 128
#define MAX_SUGGESTION_KEY 64

static const char *VALID_CONFIG_KEYS[] = {
    "DATA_DIR",
    "WORK_DIR",
    "RUN_DIR",
    "PID_FILE",
    "LOG_FILE",
    "LOG_TIMESTAMP",
    "RESTART_DELAY",
    "CLASH_SECRET",
    "API_PORT",
    "API_HOST",
    "UI_EMOJI_ENABLED",
    "CORE_USER_GROUP",
    "ENABLE_EBPF",
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

static int g_strict_mode = 0;

void config_set_strict_mode(int strict) {
    g_strict_mode = strict;
}

int config_get_strict_mode(void) {
    return g_strict_mode;
}

static int is_valid_key(const char *key) {
    for (int i = 0; VALID_CONFIG_KEYS[i] != NULL; i++) {
        if (strcmp(key, VALID_CONFIG_KEYS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int config_validate_key(const char *key, atp_config_t *cfg) {
    (void)cfg;
    if (!key || !*key) return -1;
    if (is_valid_key(key)) return 0;
    return g_strict_mode ? -1 : 0;
}

static int validate_port(int port, const char *name) {
    if (port < 1 || port > 65535) {
        LOG_ERROR("%s must be between 1-65535, got %d", name, port);
        return -1;
    }
    return 0;
}

static int validate_service_params(atp_config_t *cfg) {
    int errors = 0;

    if (cfg->service.start_timeout_sec < 1 || cfg->service.start_timeout_sec > 3600) {
        LOG_ERROR("SERVICE_START_TIMEOUT must be 1-3600 seconds, got %d",
                  cfg->service.start_timeout_sec);
        errors++;
    }
    if (cfg->service.stop_timeout_sec < 1 || cfg->service.stop_timeout_sec > 3600) {
        LOG_ERROR("SERVICE_STOP_TIMEOUT must be 1-3600 seconds, got %d",
                  cfg->service.stop_timeout_sec);
        errors++;
    }
    if (cfg->service.grace_period_sec < 0 || cfg->service.grace_period_sec > 3600) {
        LOG_ERROR("SERVICE_GRACE_PERIOD must be 0-3600 seconds, got %d",
                  cfg->service.grace_period_sec);
        errors++;
    }
    if (cfg->service.max_failures < 1 || cfg->service.max_failures > 1000) {
        LOG_ERROR("SERVICE_MAX_FAILURES must be 1-1000, got %d", cfg->service.max_failures);
        errors++;
    }
    if (cfg->service.circuit_threshold < 1 || cfg->service.circuit_threshold > 1000) {
        LOG_ERROR("SERVICE_CIRCUIT_THRESHOLD must be 1-1000, got %d",
                  cfg->service.circuit_threshold);
        errors++;
    }
    if (cfg->service.circuit_cooldown_sec < 1 || cfg->service.circuit_cooldown_sec > 86400) {
        LOG_ERROR("SERVICE_CIRCUIT_COOLDOWN must be 1-86400 seconds, got %d",
                  cfg->service.circuit_cooldown_sec);
        errors++;
    }
    if (cfg->service.health_check_interval_ms < 100 ||
        cfg->service.health_check_interval_ms > 3600000) {
        LOG_ERROR("SERVICE_HEALTH_CHECK_INTERVAL must be 100-3600000 ms, got %d",
                  cfg->service.health_check_interval_ms);
        errors++;
    }

    return errors;
}

int config_validate_values(atp_config_t *cfg) {
    int errors = 0;

    errors += validate_port(cfg->api.port, "API_PORT") ? 1 : 0;
    errors += validate_service_params(cfg);

    if (cfg->core.restart_delay < 0 || cfg->core.restart_delay > 3600) {
        LOG_ERROR("RESTART_DELAY must be between 0-3600 seconds, got %d", cfg->core.restart_delay);
        errors++;
    }

    if (errors > 0) {
        LOG_ERROR("Configuration validation found %d error(s)", errors);
        return -1;
    }

    return 0;
}
