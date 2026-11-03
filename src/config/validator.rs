use crate::config::{Config, ConfigError};
use std::net::Ipv4Addr;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConfigValueType {
    Bool,
    Int,
    String,
}

#[derive(Debug, Clone, Copy)]
pub struct ConfigKeySpec {
    pub name: &'static str,
    pub value_type: ConfigValueType,
    pub canonical_name: &'static str,
    pub allow_empty: bool,
}

pub const CONFIG_SCHEMA: &[ConfigKeySpec] = &[
    ConfigKeySpec {
        name: "DATA_DIR",
        value_type: ConfigValueType::String,
        canonical_name: "DATA_DIR",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "WORK_DIR",
        value_type: ConfigValueType::String,
        canonical_name: "DATA_DIR",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "RUN_DIR",
        value_type: ConfigValueType::String,
        canonical_name: "RUN_DIR",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "PID_FILE",
        value_type: ConfigValueType::String,
        canonical_name: "PID_FILE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "LOG_FILE",
        value_type: ConfigValueType::String,
        canonical_name: "LOG_FILE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "LOG_TIMESTAMP",
        value_type: ConfigValueType::Bool,
        canonical_name: "LOG_TIMESTAMP",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "RESTART_DELAY",
        value_type: ConfigValueType::Int,
        canonical_name: "RESTART_DELAY",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "API_SECRET",
        value_type: ConfigValueType::String,
        canonical_name: "API_SECRET",
        allow_empty: true,
    },
    ConfigKeySpec {
        name: "CLASH_SECRET",
        value_type: ConfigValueType::String,
        canonical_name: "API_SECRET",
        allow_empty: true,
    },
    ConfigKeySpec {
        name: "API_PORT",
        value_type: ConfigValueType::Int,
        canonical_name: "API_PORT",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "API_HOST",
        value_type: ConfigValueType::String,
        canonical_name: "API_HOST",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "UI_EMOJI_ENABLED",
        value_type: ConfigValueType::Bool,
        canonical_name: "UI_EMOJI_ENABLED",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "CORE_USER_GROUP",
        value_type: ConfigValueType::String,
        canonical_name: "CORE_USER_GROUP",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_START_TIMEOUT",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_START_TIMEOUT",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_STOP_TIMEOUT",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_STOP_TIMEOUT",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_GRACE_PERIOD",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_GRACE_PERIOD",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_MAX_FAILURES",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_MAX_FAILURES",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_CIRCUIT_THRESHOLD",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_CIRCUIT_THRESHOLD",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_CIRCUIT_COOLDOWN",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_CIRCUIT_COOLDOWN",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_HEALTH_CHECK_INTERVAL",
        value_type: ConfigValueType::Int,
        canonical_name: "SERVICE_HEALTH_CHECK_INTERVAL",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "SERVICE_ARGS",
        value_type: ConfigValueType::String,
        canonical_name: "SERVICE_ARGS",
        allow_empty: true,
    },
    ConfigKeySpec {
        name: "SERVICE_ENV",
        value_type: ConfigValueType::String,
        canonical_name: "SERVICE_ENV",
        allow_empty: true,
    },
    ConfigKeySpec {
        name: "VPN_AUTO_CLASH_MODE",
        value_type: ConfigValueType::Bool,
        canonical_name: "VPN_AUTO_MODE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "VPN_AUTO_MODE",
        value_type: ConfigValueType::Bool,
        canonical_name: "VPN_AUTO_MODE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "VPN_TARGET_MODE",
        value_type: ConfigValueType::String,
        canonical_name: "VPN_TARGET_MODE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "VPN_CLASH_MODE",
        value_type: ConfigValueType::String,
        canonical_name: "VPN_TARGET_MODE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "VPN_FALLBACK_MODE",
        value_type: ConfigValueType::String,
        canonical_name: "VPN_FALLBACK_MODE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "VPN_DEFAULT_MODE",
        value_type: ConfigValueType::String,
        canonical_name: "VPN_FALLBACK_MODE",
        allow_empty: false,
    },
    ConfigKeySpec {
        name: "VPN_BYPASS_ROUTES",
        value_type: ConfigValueType::String,
        canonical_name: "VPN_BYPASS_ROUTES",
        allow_empty: true,
    },
    ConfigKeySpec {
        name: "VPN_BYPASS_PREF",
        value_type: ConfigValueType::Int,
        canonical_name: "VPN_BYPASS_PREF",
        allow_empty: false,
    },
];

pub fn config_schema_find(key: &str) -> Option<&'static ConfigKeySpec> {
    if key.is_empty() {
        return None;
    }
    CONFIG_SCHEMA.iter().find(|spec| spec.name == key)
}

pub fn validate_port(port: i32) -> bool {
    (1..=65535).contains(&port)
}

pub fn config_validate_values(cfg: &Config) -> Result<(), ConfigError> {
    if !validate_port(cfg.api.port) {
        return Err(ConfigError::Validation(format!(
            "invalid API port: {}",
            cfg.api.port
        )));
    }

    if cfg.api.host.parse::<Ipv4Addr>().is_err() {
        return Err(ConfigError::Validation(format!(
            "invalid API host IPv4 address: '{}'",
            cfg.api.host
        )));
    }

    if !(1..=3600).contains(&cfg.service.start_timeout_sec) {
        return Err(ConfigError::Validation(format!(
            "start_timeout_sec {} out of range [1, 3600]",
            cfg.service.start_timeout_sec
        )));
    }

    if !(1..=3600).contains(&cfg.service.stop_timeout_sec) {
        return Err(ConfigError::Validation(format!(
            "stop_timeout_sec {} out of range [1, 3600]",
            cfg.service.stop_timeout_sec
        )));
    }

    if !(0..=3600).contains(&cfg.service.grace_period_sec) {
        return Err(ConfigError::Validation(format!(
            "grace_period_sec {} out of range [0, 3600]",
            cfg.service.grace_period_sec
        )));
    }

    if !(1..=1000).contains(&cfg.service.max_failures) {
        return Err(ConfigError::Validation(format!(
            "max_failures {} out of range [1, 1000]",
            cfg.service.max_failures
        )));
    }

    if !(1..=1000).contains(&cfg.service.circuit_threshold) {
        return Err(ConfigError::Validation(format!(
            "circuit_threshold {} out of range [1, 1000]",
            cfg.service.circuit_threshold
        )));
    }

    if !(1..=86400).contains(&cfg.service.circuit_cooldown_sec) {
        return Err(ConfigError::Validation(format!(
            "circuit_cooldown_sec {} out of range [1, 86400]",
            cfg.service.circuit_cooldown_sec
        )));
    }

    if !(100..=3600000).contains(&cfg.service.health_check_interval_ms) {
        return Err(ConfigError::Validation(format!(
            "health_check_interval_ms {} out of range [100, 3600000]",
            cfg.service.health_check_interval_ms
        )));
    }

    if !(0..=3600).contains(&cfg.service.restart_delay_sec) {
        return Err(ConfigError::Validation(format!(
            "restart_delay_sec {} out of range [0, 3600]",
            cfg.service.restart_delay_sec
        )));
    }

    Ok(())
}
