use std::io::BufRead;
use std::net::{Ipv4Addr, Ipv6Addr};
use std::path::Path;

use crate::config::validator::{config_schema_find, ConfigKeySpec, ConfigValueType};
use crate::config::{
    Config, ConfigError, ConfigPresence, AF_INET, AF_INET6, ATP_MAX_VPN_BYPASS_ROUTES,
};

pub fn parse_vpn_bypass_routes(v: &str, cfg: &mut Config) -> Result<(), ()> {
    cfg.interface.vpn_bypass_route_count = 0;
    cfg.interface.vpn_bypass_routes =
        [crate::config::VpnBypassRoute::default(); ATP_MAX_VPN_BYPASS_ROUTES];

    let trimmed = v.trim_matches(|c: char| c.is_ascii_whitespace() || c == ',');
    if trimmed.is_empty() {
        return Ok(());
    }

    if v.len() >= 1024 {
        return Err(());
    }

    let tokens = v
        .split(|c: char| c.is_ascii_whitespace() || c == ',')
        .filter(|s| !s.is_empty());
    let mut count = 0;

    for token in tokens {
        let (addr_str, prefix_str) = match token.split_once('/') {
            Some(pair) => pair,
            None => return Err(()),
        };
        if addr_str.is_empty() || prefix_str.is_empty() {
            return Err(());
        }
        if !prefix_str.chars().all(|c| c.is_ascii_digit()) {
            return Err(());
        }
        let plen: u8 = match prefix_str.parse() {
            Ok(p) if p > 0 => p,
            _ => return Err(()),
        };

        let (family, a) = if let Ok(ipv4) = addr_str.parse::<Ipv4Addr>() {
            if plen > 32 {
                return Err(());
            }
            let octets = ipv4.octets();
            let mut addr = [0u8; 16];
            addr[..4].copy_from_slice(&octets);
            for (i, byte) in addr[..4].iter_mut().enumerate() {
                let plen_val = plen as usize;
                if plen_val >= (i + 1) * 8 {
                    // keep byte
                } else if plen_val <= i * 8 {
                    *byte = 0;
                } else {
                    let bits = plen_val - i * 8;
                    let mask = 0xFFu8 << (8 - bits);
                    *byte &= mask;
                }
            }
            (AF_INET, addr)
        } else if let Ok(ipv6) = addr_str.parse::<Ipv6Addr>() {
            if plen > 128 {
                return Err(());
            }
            let mut addr = ipv6.octets();
            for (i, byte) in addr.iter_mut().enumerate() {
                let plen_val = plen as usize;
                if plen_val >= (i + 1) * 8 {
                    // keep byte
                } else if plen_val <= i * 8 {
                    *byte = 0;
                } else {
                    let bits = plen_val - i * 8;
                    let mask = 0xFFu8 << (8 - bits);
                    *byte &= mask;
                }
            }
            (AF_INET6, addr)
        } else {
            return Err(());
        };

        let mut duplicate = false;
        for r in &cfg.interface.vpn_bypass_routes[..count] {
            if r.family == family && r.prefix_len == plen {
                let cmp_len = if family == AF_INET { 4 } else { 16 };
                if r.addr[..cmp_len] == a[..cmp_len] {
                    duplicate = true;
                    break;
                }
            }
        }

        if !duplicate {
            if count >= ATP_MAX_VPN_BYPASS_ROUTES {
                return Err(());
            }
            cfg.interface.vpn_bypass_routes[count] = crate::config::VpnBypassRoute {
                family,
                addr: a,
                prefix_len: plen,
            };
            count += 1;
        }
    }

    cfg.interface.vpn_bypass_route_count = count;
    Ok(())
}

fn parse_key_value(spec: &ConfigKeySpec, k: &str, v: &str, cfg: &mut Config) -> Result<(), ()> {
    if !spec.allow_empty && v.is_empty() {
        return Err(());
    }

    match spec.value_type {
        ConfigValueType::Int => {
            let value: i32 = match v.parse() {
                Ok(val) => val,
                Err(_) => return Err(()),
            };
            match k {
                "RESTART_DELAY" => cfg.service.restart_delay_sec = value,
                "API_PORT" => cfg.api.port = value,
                "SERVICE_START_TIMEOUT" => cfg.service.start_timeout_sec = value,
                "SERVICE_STOP_TIMEOUT" => cfg.service.stop_timeout_sec = value,
                "SERVICE_GRACE_PERIOD" => cfg.service.grace_period_sec = value,
                "SERVICE_MAX_FAILURES" => cfg.service.max_failures = value,
                "SERVICE_CIRCUIT_THRESHOLD" => cfg.service.circuit_threshold = value,
                "SERVICE_CIRCUIT_COOLDOWN" => cfg.service.circuit_cooldown_sec = value,
                "SERVICE_HEALTH_CHECK_INTERVAL" => cfg.service.health_check_interval_ms = value,
                "VPN_BYPASS_PREF" => {
                    if !(1..=32765).contains(&value) {
                        return Err(());
                    }
                    cfg.interface.vpn_bypass_pref = value;
                }
                _ => return Err(()),
            }
            Ok(())
        }
        ConfigValueType::Bool => {
            let int_val: i32 = match v.parse() {
                Ok(val) => val,
                Err(_) => return Err(()),
            };
            if int_val != 0 && int_val != 1 {
                return Err(());
            }
            let b = int_val != 0;
            match k {
                "LOG_TIMESTAMP" => cfg.core.log_timestamp = b,
                "UI_EMOJI_ENABLED" => cfg.core.ui_emoji_enabled = b,
                "VPN_AUTO_CLASH_MODE" | "VPN_AUTO_MODE" => cfg.interface.vpn_auto_mode = b,
                _ => return Err(()),
            }
            Ok(())
        }
        ConfigValueType::String => {
            match k {
                "API_SECRET" | "CLASH_SECRET" => {
                    if v.len() >= 128 {
                        return Err(());
                    }
                    cfg.api.secret = v.to_string();
                }
                "API_HOST" => {
                    if v.len() >= 64 {
                        return Err(());
                    }
                    cfg.api.host = v.to_string();
                }
                "VPN_TARGET_MODE" | "VPN_CLASH_MODE" => {
                    if v.len() >= 64 {
                        return Err(());
                    }
                    cfg.interface.vpn_target_mode = v.to_string();
                }
                "VPN_FALLBACK_MODE" | "VPN_DEFAULT_MODE" => {
                    if v.len() >= 64 {
                        return Err(());
                    }
                    cfg.interface.vpn_fallback_mode = v.to_string();
                }
                "VPN_BYPASS_ROUTES" => {
                    parse_vpn_bypass_routes(v, cfg)?;
                }
                "DATA_DIR" | "WORK_DIR" => {
                    if v.len() >= 4096 {
                        return Err(());
                    }
                    cfg.core.data_dir = v.to_string();
                }
                "RUN_DIR" => {
                    if v.len() >= 4096 {
                        return Err(());
                    }
                    cfg.core.run_dir = v.to_string();
                }
                "PID_FILE" => {
                    if v.len() >= 4096 {
                        return Err(());
                    }
                    cfg.core.pid_file = v.to_string();
                }
                "LOG_FILE" => {
                    if v.len() >= 4096 {
                        return Err(());
                    }
                    cfg.core.log_file = v.to_string();
                }
                "CORE_USER_GROUP" => {
                    let (user, group) = match v.split_once(':') {
                        Some(p) => p,
                        None => return Err(()),
                    };
                    if user.is_empty()
                        || group.is_empty()
                        || group.contains(':')
                        || user.len() >= 64
                        || group.len() >= 64
                    {
                        return Err(());
                    }
                    cfg.core.core_user = user.to_string();
                    cfg.core.core_group = group.to_string();
                }
                "SERVICE_ARGS" => {
                    if v.len() >= 256 {
                        return Err(());
                    }
                    cfg.service.args = v.to_string();
                }
                "SERVICE_ENV" => {
                    if v.len() >= 256 {
                        return Err(());
                    }
                    cfg.service.env = v.to_string();
                }
                _ => return Err(()),
            }
            Ok(())
        }
    }
}

pub fn parse_file<P: AsRef<Path>>(
    path: P,
    cfg: &mut Config,
    presence: &mut ConfigPresence,
) -> Result<(), ConfigError> {
    let file = std::fs::File::open(path.as_ref()).map_err(|e| {
        if e.kind() == std::io::ErrorKind::NotFound {
            ConfigError::NotFound(path.as_ref().to_string_lossy().into_owned())
        } else {
            ConfigError::Io(e)
        }
    })?;

    let mut reader = std::io::BufReader::new(file);
    let mut line_buf = Vec::new();
    let mut line_no = 0;
    let mut seen_keys: Vec<&'static str> = Vec::new();

    loop {
        line_buf.clear();
        let bytes_read = reader
            .read_until(b'\n', &mut line_buf)
            .map_err(ConfigError::Io)?;
        if bytes_read == 0 {
            break;
        }
        line_no += 1;

        // Check line length without newline
        let mut len = line_buf.len();
        if len > 0 && line_buf[len - 1] == b'\n' {
            len -= 1;
            if len > 0 && line_buf[len - 1] == b'\r' {
                len -= 1;
            }
        }

        if len > 1023 {
            return Err(ConfigError::Parse {
                line: line_no,
                key: None,
                reason: "line exceeds 1023 bytes".to_string(),
            });
        }

        let line_str = match std::str::from_utf8(&line_buf[..len]) {
            Ok(s) => s,
            Err(_) => {
                return Err(ConfigError::Parse {
                    line: line_no,
                    key: None,
                    reason: "invalid UTF-8".to_string(),
                });
            }
        };

        let trimmed = line_str.trim_matches(|c: char| c.is_ascii_whitespace());
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }

        let (raw_k, raw_v) = match trimmed.split_once('=') {
            Some(pair) => pair,
            None => {
                return Err(ConfigError::Parse {
                    line: line_no,
                    key: None,
                    reason: "expected KEY=VALUE".to_string(),
                });
            }
        };

        let k = raw_k.trim_matches(|c: char| c.is_ascii_whitespace());
        let v = raw_v.trim_matches(|c: char| c.is_ascii_whitespace());

        if k.is_empty() {
            return Err(ConfigError::Parse {
                line: line_no,
                key: None,
                reason: "empty key".to_string(),
            });
        }

        let v_parsed: &str;
        if (v.starts_with('"') || v.starts_with('\'')) && v.len() >= 2 {
            let quote_char = match v.chars().next() {
                Some(c) => c,
                None => {
                    return Err(ConfigError::Parse {
                        line: line_no,
                        key: Some(k.to_string()),
                        reason: "unterminated quote".to_string(),
                    });
                }
            };
            if v.ends_with(quote_char) {
                v_parsed = &v[1..v.len() - 1];
            } else {
                return Err(ConfigError::Parse {
                    line: line_no,
                    key: Some(k.to_string()),
                    reason: "unterminated quote".to_string(),
                });
            }
        } else if v.starts_with('"') || v.starts_with('\'') {
            return Err(ConfigError::Parse {
                line: line_no,
                key: Some(k.to_string()),
                reason: "unterminated quote".to_string(),
            });
        } else {
            v_parsed = v;
        }

        let spec = match config_schema_find(k) {
            Some(s) => s,
            None => {
                return Err(ConfigError::Parse {
                    line: line_no,
                    key: Some(k.to_string()),
                    reason: "unknown key".to_string(),
                });
            }
        };

        if spec.canonical_name == "API_PORT" {
            presence.api_port = true;
        } else if spec.canonical_name == "API_HOST" {
            presence.api_host = true;
        } else if spec.canonical_name == "API_SECRET" {
            presence.api_secret = true;
        }

        if seen_keys.contains(&spec.canonical_name) {
            return Err(ConfigError::Parse {
                line: line_no,
                key: Some(k.to_string()),
                reason: "duplicate key".to_string(),
            });
        }
        seen_keys.push(spec.canonical_name);

        if parse_key_value(spec, k, v_parsed, cfg).is_err() {
            return Err(ConfigError::Parse {
                line: line_no,
                key: Some(k.to_string()),
                reason: "invalid value".to_string(),
            });
        }
    }

    Ok(())
}
