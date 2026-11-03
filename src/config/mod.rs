pub mod atp_conf;
pub mod validator;

#[cfg(test)]
mod tests;

use std::path::Path;

pub const ATP_MAX_VPN_BYPASS_ROUTES: usize = 16;
pub const AF_INET: i32 = 2;
pub const AF_INET6: i32 = 10;
pub const DEFAULT_API_PORT: i32 = 9080;
pub const DEFAULT_API_HOST: &str = "127.0.0.1";
pub const ATP_RUN_DIR: &str = "run";
pub const ATP_PID_FILE: &str = "run/atpd.pid";
pub const ATP_LOG_FILE: &str = "run/atp.log";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct VpnBypassRoute {
    pub family: i32,
    pub addr: [u8; 16],
    pub prefix_len: u8,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CoreConfig {
    pub ui_emoji_enabled: bool,
    pub log_timestamp: bool,
    pub data_dir: String,
    pub run_dir: String,
    pub core_user: String,
    pub core_group: String,
    pub pid_file: String,
    pub log_file: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InterfaceConfig {
    pub vpn_auto_mode: bool,
    pub vpn_target_mode: String,
    pub vpn_fallback_mode: String,
    pub vpn_bypass_routes: [VpnBypassRoute; ATP_MAX_VPN_BYPASS_ROUTES],
    pub vpn_bypass_route_count: usize,
    pub vpn_bypass_pref: i32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServiceConfig {
    pub restart_delay_sec: i32,
    pub start_timeout_sec: i32,
    pub stop_timeout_sec: i32,
    // Accepted for old atp.conf files; nothing uses it.
    pub grace_period_sec: i32,
    pub max_failures: i32,
    pub circuit_threshold: i32,
    pub circuit_cooldown_sec: i32,
    pub health_check_interval_ms: i32,
    pub args: String,
    pub env: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApiConfig {
    pub port: i32,
    pub host: String,
    pub secret: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub core: CoreConfig,
    pub interface: InterfaceConfig,
    pub service: ServiceConfig,
    pub api: ApiConfig,
}

#[derive(Debug, Default, Clone, Copy)]
pub struct ConfigPresence {
    pub api_port: bool,
    pub api_host: bool,
    pub api_secret: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ReloadChanges(pub u32);

impl ReloadChanges {
    pub const NONE: Self = Self(0);
    pub const HOT: Self = Self(1 << 0);
    pub const STATIC: Self = Self(1 << 1);
    pub const REQUIRES_RESTART: Self = Self(1 << 2);

    pub fn is_none(&self) -> bool {
        self.0 == 0
    }

    pub fn contains(&self, other: Self) -> bool {
        (self.0 & other.0) == other.0
    }

    pub fn bits(&self) -> u32 {
        self.0
    }
}

impl std::ops::BitOr for ReloadChanges {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl std::ops::BitOrAssign for ReloadChanges {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl std::ops::BitAnd for ReloadChanges {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
    }
}

impl std::ops::BitAndAssign for ReloadChanges {
    fn bitand_assign(&mut self, rhs: Self) {
        self.0 &= rhs.0;
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReloadClassification {
    pub changes: ReloadChanges,
    pub changed_fields: String,
}

impl std::ops::Deref for ReloadClassification {
    type Target = ReloadChanges;
    fn deref(&self) -> &Self::Target {
        &self.changes
    }
}

impl std::ops::BitAnd<ReloadChanges> for ReloadClassification {
    type Output = ReloadChanges;
    fn bitand(self, rhs: ReloadChanges) -> ReloadChanges {
        self.changes & rhs
    }
}

impl std::ops::BitAnd<ReloadChanges> for &ReloadClassification {
    type Output = ReloadChanges;
    fn bitand(self, rhs: ReloadChanges) -> ReloadChanges {
        self.changes & rhs
    }
}

impl PartialEq<ReloadChanges> for ReloadClassification {
    fn eq(&self, other: &ReloadChanges) -> bool {
        &self.changes == other
    }
}

#[derive(Debug)]
pub enum ConfigError {
    Parse {
        line: usize,
        key: Option<String>,
        reason: String,
    },
    Validation(String),
    NotFound(String),
    Io(std::io::Error),
    Json(String),
    InvalidArgument(String),
}

impl std::fmt::Display for ConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ConfigError::Parse {
                line,
                key: Some(key),
                reason,
            } => {
                write!(
                    f,
                    "Invalid configuration at line {} (key {}): {}",
                    line, key, reason
                )
            }
            ConfigError::Parse {
                line,
                key: None,
                reason,
            } => {
                write!(f, "Invalid configuration at line {}: {}", line, reason)
            }
            ConfigError::Validation(msg) => write!(f, "Configuration validation error: {}", msg),
            ConfigError::NotFound(path) => write!(f, "Configuration file not found: {}", path),
            ConfigError::Io(err) => write!(f, "I/O error: {}", err),
            ConfigError::Json(msg) => write!(f, "JSON configuration error: {}", msg),
            ConfigError::InvalidArgument(msg) => write!(f, "Invalid argument: {}", msg),
        }
    }
}

impl std::error::Error for ConfigError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            ConfigError::Io(err) => Some(err),
            _ => None,
        }
    }
}

impl From<std::io::Error> for ConfigError {
    fn from(err: std::io::Error) -> Self {
        if err.kind() == std::io::ErrorKind::NotFound {
            ConfigError::NotFound(err.to_string())
        } else {
            ConfigError::Io(err)
        }
    }
}

fn get_app_dir() -> String {
    if let Ok(exe_path) = std::env::current_exe() {
        if let Some(parent) = exe_path.parent() {
            if parent.file_name().and_then(|s| s.to_str()) == Some("bin") {
                if let Some(grandparent) = parent.parent() {
                    return grandparent.to_string_lossy().into_owned();
                }
            }
            return parent.to_string_lossy().into_owned();
        }
    }
    if Path::new("/data/adb/atp").exists() {
        return "/data/adb/atp".to_string();
    }
    ".".to_string()
}

fn get_default_group() -> String {
    #[cfg(target_os = "android")]
    {
        "net_admin".to_string()
    }
    #[cfg(not(target_os = "android"))]
    {
        if let Ok(content) = std::fs::read_to_string("/etc/group") {
            for line in content.lines() {
                if line.starts_with("net_admin:") {
                    return "net_admin".to_string();
                }
            }
        }
        "root".to_string()
    }
}

impl Default for Config {
    fn default() -> Self {
        Self {
            core: CoreConfig {
                ui_emoji_enabled: true,
                log_timestamp: true,
                data_dir: get_app_dir(),
                run_dir: ATP_RUN_DIR.to_string(),
                core_user: "root".to_string(),
                core_group: get_default_group(),
                pid_file: ATP_PID_FILE.to_string(),
                log_file: ATP_LOG_FILE.to_string(),
            },
            interface: InterfaceConfig {
                vpn_auto_mode: true,
                vpn_target_mode: "Google VPN".to_string(),
                vpn_fallback_mode: "Rule".to_string(),
                vpn_bypass_routes: [VpnBypassRoute::default(); ATP_MAX_VPN_BYPASS_ROUTES],
                vpn_bypass_route_count: 0,
                vpn_bypass_pref: 12400,
            },
            service: ServiceConfig {
                restart_delay_sec: 2,
                start_timeout_sec: 30,
                stop_timeout_sec: 10,
                grace_period_sec: 3,
                max_failures: 5,
                circuit_threshold: 5,
                circuit_cooldown_sec: 60,
                health_check_interval_ms: 5000,
                args: String::new(),
                env: String::new(),
            },
            api: ApiConfig {
                port: DEFAULT_API_PORT,
                host: DEFAULT_API_HOST.to_string(),
                secret: String::new(),
            },
        }
    }
}

fn apply_run_dir_defaults(cfg: &mut Config) -> Result<(), ConfigError> {
    if cfg.core.run_dir.is_empty() || cfg.core.run_dir == ATP_RUN_DIR {
        return Ok(());
    }

    if cfg.core.pid_file == ATP_PID_FILE {
        let new_pid = format!("{}/atpd.pid", cfg.core.run_dir);
        if new_pid.len() >= 4096 {
            return Err(ConfigError::Validation(
                "RUN_DIR leaves no room for default file paths".to_string(),
            ));
        }
        cfg.core.pid_file = new_pid;
    }
    if cfg.core.log_file == ATP_LOG_FILE {
        let new_log = format!("{}/atp.log", cfg.core.run_dir);
        if new_log.len() >= 4096 {
            return Err(ConfigError::Validation(
                "RUN_DIR leaves no room for default file paths".to_string(),
            ));
        }
        cfg.core.log_file = new_log;
    }
    Ok(())
}

fn sync_from_singbox_json(cfg: &mut Config, presence: &ConfigPresence) -> Result<(), ConfigError> {
    let conf_path = Path::new(&cfg.core.data_dir).join("config.json");
    if !conf_path.exists() {
        return Ok(());
    }

    let content = std::fs::read_to_string(&conf_path)
        .map_err(|e| ConfigError::Json(format!("{}: {}", conf_path.display(), e)))?;

    let doc: serde_json::Value = match serde_json::from_str(&content) {
        Ok(v) => v,
        Err(e) => return Err(ConfigError::Json(e.to_string())),
    };

    let root = match doc.as_object() {
        Some(r) => r,
        None => return Ok(()),
    };

    let mut api_service = None;

    if let Some(services) = root.get("services").and_then(|v| v.as_array()) {
        for svc in services {
            if let Some(svc_obj) = svc.as_object() {
                if svc_obj.get("type").and_then(|t| t.as_str()) == Some("api") {
                    api_service = Some(svc_obj);
                    break;
                }
            }
        }
    }

    if api_service.is_none() {
        if let Some(top_api) = root.get("api").and_then(|v| v.as_object()) {
            api_service = Some(top_api);
        }
    }

    if api_service.is_none() {
        if let Some(exp_api) = root
            .get("experimental")
            .and_then(|e| e.get("api"))
            .and_then(|v| v.as_object())
        {
            api_service = Some(exp_api);
        }
    }

    if let Some(svc) = api_service {
        if let Some(listen_val) = svc.get("listen") {
            if let Some(listen_str) = listen_val.as_str() {
                if !listen_str.is_empty() {
                    if let Some(colon_pos) = listen_str.rfind(':') {
                        let port_str = &listen_str[colon_pos + 1..];
                        let port: i32 = match port_str.parse() {
                            Ok(p) => p,
                            Err(_) => {
                                return Err(ConfigError::Validation(
                                    "invalid listen port in config.json".to_string(),
                                ));
                            }
                        };
                        if !validator::validate_port(port) {
                            return Err(ConfigError::Validation(
                                "invalid listen port in config.json".to_string(),
                            ));
                        }
                        if !presence.api_port {
                            cfg.api.port = port;
                        }
                        if colon_pos > 0 && !presence.api_host {
                            let host_str = &listen_str[..colon_pos];
                            if host_str.len() >= 64 {
                                return Err(ConfigError::Validation(
                                    "API host too long in config.json".to_string(),
                                ));
                            }
                            cfg.api.host = host_str.to_string();
                        }
                    } else if !presence.api_host {
                        if listen_str.len() >= 64 {
                            return Err(ConfigError::Validation(
                                "API host too long in config.json".to_string(),
                            ));
                        }
                        cfg.api.host = listen_str.to_string();
                    }
                }
            }
        }

        let port_val = svc.get("listen_port").or_else(|| svc.get("port"));
        if let Some(pv) = port_val {
            let p = match pv.as_i64().and_then(|v| i32::try_from(v).ok()) {
                Some(val) => val,
                None => {
                    return Err(ConfigError::Validation(
                        "invalid port type in config.json".to_string(),
                    ));
                }
            };
            if !validator::validate_port(p) {
                return Err(ConfigError::Validation(
                    "invalid port in config.json".to_string(),
                ));
            }
            if !presence.api_port {
                cfg.api.port = p;
            }
        }

        if let Some(secret_val) = svc.get("secret") {
            let sec = match secret_val.as_str() {
                Some(s) => s,
                None => {
                    return Err(ConfigError::Validation(
                        "invalid secret type in config.json".to_string(),
                    ));
                }
            };
            if !sec.is_empty() && !presence.api_secret {
                if sec.len() >= 128 {
                    return Err(ConfigError::Validation(
                        "secret too long in config.json".to_string(),
                    ));
                }
                cfg.api.secret = sec.to_string();
            }
        }
    }

    Ok(())
}

impl Config {
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self, ConfigError> {
        let path_ref = path.as_ref();
        if path_ref.as_os_str().is_empty() {
            return Err(ConfigError::InvalidArgument(
                "empty configuration path".to_string(),
            ));
        }
        let mut cfg = Config::default();
        let mut presence = ConfigPresence::default();
        atp_conf::parse_file(path_ref, &mut cfg, &mut presence)?;
        apply_run_dir_defaults(&mut cfg)?;
        sync_from_singbox_json(&mut cfg, &presence)?;
        validator::config_validate_values(&cfg)?;
        Ok(cfg)
    }

    pub fn prepare_reload<P: AsRef<Path>>(source_path: P) -> Result<Self, ConfigError> {
        let path_ref = source_path.as_ref();
        if path_ref.as_os_str().is_empty() {
            return Err(ConfigError::InvalidArgument(
                "empty configuration path".to_string(),
            ));
        }
        if !path_ref.exists() {
            return Err(ConfigError::NotFound(
                path_ref.to_string_lossy().into_owned(),
            ));
        }
        let mut new_config = Config::default();
        let mut presence = ConfigPresence::default();
        atp_conf::parse_file(path_ref, &mut new_config, &mut presence)?;
        apply_run_dir_defaults(&mut new_config)?;
        sync_from_singbox_json(&mut new_config, &presence)?;
        validator::config_validate_values(&new_config)?;
        Ok(new_config)
    }
}

pub fn load<P: AsRef<Path>>(path: P) -> Result<Config, ConfigError> {
    Config::load(path)
}

pub fn prepare_reload<P: AsRef<Path>>(source_path: P) -> Result<Config, ConfigError> {
    Config::prepare_reload(source_path)
}

pub fn classify_reload(current: &Config, candidate: &Config) -> ReloadClassification {
    let mut changes = ReloadChanges::NONE;
    let mut restart_fields = Vec::new();

    if current.core.ui_emoji_enabled != candidate.core.ui_emoji_enabled {
        changes |= ReloadChanges::HOT;
    }
    if current.service.start_timeout_sec != candidate.service.start_timeout_sec {
        changes |= ReloadChanges::HOT;
    }
    if current.service.stop_timeout_sec != candidate.service.stop_timeout_sec {
        changes |= ReloadChanges::HOT;
    }
    if current.service.max_failures != candidate.service.max_failures {
        changes |= ReloadChanges::HOT;
    }
    if current.service.health_check_interval_ms != candidate.service.health_check_interval_ms {
        changes |= ReloadChanges::HOT;
    }

    if current.core.log_timestamp != candidate.core.log_timestamp {
        changes |= ReloadChanges::STATIC;
    }
    if current.service.restart_delay_sec != candidate.service.restart_delay_sec {
        changes |= ReloadChanges::STATIC;
    }
    if current.service.grace_period_sec != candidate.service.grace_period_sec {
        changes |= ReloadChanges::STATIC;
    }

    if current.core.data_dir != candidate.core.data_dir {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("DATA_DIR");
    }
    if current.core.run_dir != candidate.core.run_dir {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("RUN_DIR");
    }
    if current.core.pid_file != candidate.core.pid_file {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("PID_FILE");
    }
    if current.core.log_file != candidate.core.log_file {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("LOG_FILE");
    }
    if current.core.core_user != candidate.core.core_user
        || current.core.core_group != candidate.core.core_group
    {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("CORE_USER_GROUP");
    }
    if current.service.args != candidate.service.args {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("SERVICE_ARGS");
    }
    if current.service.env != candidate.service.env {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("SERVICE_ENV");
    }
    if current.api.host != candidate.api.host {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("API_HOST");
    }
    if current.api.port != candidate.api.port {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("API_PORT");
    }
    if current.api.secret != candidate.api.secret {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("API_SECRET");
    }
    if current.interface.vpn_auto_mode != candidate.interface.vpn_auto_mode {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("VPN_AUTO_MODE");
    }
    if current.interface.vpn_target_mode != candidate.interface.vpn_target_mode {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("VPN_TARGET_MODE");
    }
    if current.interface.vpn_fallback_mode != candidate.interface.vpn_fallback_mode {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("VPN_FALLBACK_MODE");
    }

    if current.interface.vpn_bypass_route_count != candidate.interface.vpn_bypass_route_count
        || current.interface.vpn_bypass_routes != candidate.interface.vpn_bypass_routes
    {
        changes |= ReloadChanges::HOT;
    }
    if current.interface.vpn_bypass_pref != candidate.interface.vpn_bypass_pref {
        changes |= ReloadChanges::HOT;
    }

    if current.service.circuit_threshold != candidate.service.circuit_threshold {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("SERVICE_CIRCUIT_THRESHOLD");
    }
    if current.service.circuit_cooldown_sec != candidate.service.circuit_cooldown_sec {
        changes |= ReloadChanges::REQUIRES_RESTART;
        restart_fields.push("SERVICE_CIRCUIT_COOLDOWN");
    }

    ReloadClassification {
        changes,
        changed_fields: restart_fields.join(", "),
    }
}
