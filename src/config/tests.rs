#![cfg(test)]

use crate::config::{
    classify_reload, load, prepare_reload, Config, ConfigError, ReloadChanges, AF_INET, AF_INET6,
};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TempFileGuard(PathBuf);
impl Drop for TempFileGuard {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

struct TempDirGuard(PathBuf);
impl Drop for TempDirGuard {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn create_temp_conf(content: &str) -> (PathBuf, TempFileGuard) {
    let count = TEMP_COUNTER.fetch_add(1, Ordering::SeqCst);
    let path = std::env::temp_dir().join(format!(
        "atpd_test_cfg_{}_{}.conf",
        std::process::id(),
        count
    ));
    if let Err(e) = std::fs::write(&path, content) {
        panic!("failed to write temp file: {}", e);
    }
    let guard = TempFileGuard(path.clone());
    (path, guard)
}

fn create_temp_dir() -> (PathBuf, TempDirGuard) {
    let count = TEMP_COUNTER.fetch_add(1, Ordering::SeqCst);
    let path = std::env::temp_dir().join(format!("atpd_test_dir_{}_{}", std::process::id(), count));
    if let Err(e) = std::fs::create_dir_all(&path) {
        panic!("failed to create temp dir: {}", e);
    }
    let guard = TempDirGuard(path.clone());
    (path, guard)
}

// Mirrors test_vpn_bypass_config.c: Case 1 - Default configuration
#[test]
fn test_vpn_bypass_config_default() {
    let (path, _guard) = create_temp_conf("# empty config\n");
    let cfg = match load(&path) {
        Ok(c) => c,
        Err(e) => panic!("load failed: {}", e),
    };
    assert_eq!(cfg.interface.vpn_bypass_route_count, 0);
    assert_eq!(cfg.interface.vpn_bypass_pref, 12400);
}

// Mirrors test_vpn_bypass_config.c: Case 2 - Valid mixed list
#[test]
fn test_vpn_bypass_config_valid_mixed() {
    let valid_mixed = "VPN_BYPASS_ROUTES=\"100.64.0.0/10, fd7a:115c:a1e0::/48 192.168.88.0/24\"\n";
    let (path, _guard) = create_temp_conf(valid_mixed);
    let cfg = match load(&path) {
        Ok(c) => c,
        Err(e) => panic!("load failed: {}", e),
    };
    assert_eq!(cfg.interface.vpn_bypass_route_count, 3);

    // Route 0: 100.64.0.0/10 (IPv4)
    assert_eq!(cfg.interface.vpn_bypass_routes[0].family, AF_INET);
    assert_eq!(cfg.interface.vpn_bypass_routes[0].prefix_len, 10);
    assert_eq!(
        &cfg.interface.vpn_bypass_routes[0].addr[..4],
        &[100, 64, 0, 0]
    );

    // Route 1: fd7a:115c:a1e0::/48 (IPv6)
    assert_eq!(cfg.interface.vpn_bypass_routes[1].family, AF_INET6);
    assert_eq!(cfg.interface.vpn_bypass_routes[1].prefix_len, 48);
    let expected_v6: [u8; 16] = [
        0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ];
    assert_eq!(cfg.interface.vpn_bypass_routes[1].addr, expected_v6);

    // Route 2: 192.168.88.0/24 (IPv4)
    assert_eq!(cfg.interface.vpn_bypass_routes[2].family, AF_INET);
    assert_eq!(cfg.interface.vpn_bypass_routes[2].prefix_len, 24);
    assert_eq!(
        &cfg.interface.vpn_bypass_routes[2].addr[..4],
        &[192, 168, 88, 0]
    );
}

// Mirrors test_vpn_bypass_config.c: Case 3 - Host bits masked off: 192.168.88.7/24 -> 192.168.88.0
#[test]
fn test_vpn_bypass_config_host_bits_masked() {
    let (path, _guard) = create_temp_conf("VPN_BYPASS_ROUTES=\"192.168.88.7/24\"\n");
    let cfg = match load(&path) {
        Ok(c) => c,
        Err(e) => panic!("load failed: {}", e),
    };
    assert_eq!(cfg.interface.vpn_bypass_route_count, 1);
    assert_eq!(cfg.interface.vpn_bypass_routes[0].family, AF_INET);
    assert_eq!(cfg.interface.vpn_bypass_routes[0].prefix_len, 24);
    assert_eq!(
        &cfg.interface.vpn_bypass_routes[0].addr[..4],
        &[192, 168, 88, 0]
    );
}

// Mirrors test_vpn_bypass_config.c: Case 4 - Empty value -> 0 routes
#[test]
fn test_vpn_bypass_config_empty_value() {
    let (path, _guard) = create_temp_conf("VPN_BYPASS_ROUTES=\"\"\n");
    let cfg = match load(&path) {
        Ok(c) => c,
        Err(e) => panic!("load failed: {}", e),
    };
    assert_eq!(cfg.interface.vpn_bypass_route_count, 0);
}

// Mirrors test_vpn_bypass_config.c: Case 5 - Custom pref parsed: VPN_BYPASS_PREF=12000
#[test]
fn test_vpn_bypass_config_custom_pref() {
    let (path, _guard) = create_temp_conf("VPN_BYPASS_PREF=12000\n");
    let cfg = match load(&path) {
        Ok(c) => c,
        Err(e) => panic!("load failed: {}", e),
    };
    assert_eq!(cfg.interface.vpn_bypass_pref, 12000);
}

// Mirrors test_vpn_bypass_config.c: Case 6 - Invalid cases rejected with ATP_ERR_CONFIG
#[test]
fn test_vpn_bypass_config_invalid_cases() {
    let check_invalid = |content: &str| {
        let (p, _g) = create_temp_conf(content);
        assert!(load(&p).is_err(), "expected error for: {}", content);
    };

    check_invalid("VPN_BYPASS_ROUTES=\"100.64.0.0\"\n");
    check_invalid("VPN_BYPASS_ROUTES=\"100.64.0.0/33\"\n");
    check_invalid("VPN_BYPASS_ROUTES=\"0.0.0.0/0\"\n");
    check_invalid("VPN_BYPASS_ROUTES=\"fd7a::/129\"\n");
    check_invalid("VPN_BYPASS_ROUTES=\"nonsense/8\"\n");

    // Exactly 16 entries succeeds
    let sixteen_entries = "VPN_BYPASS_ROUTES=\"10.0.0.0/24 10.0.1.0/24 10.0.2.0/24 10.0.3.0/24 \
        10.0.4.0/24 10.0.5.0/24 10.0.6.0/24 10.0.7.0/24 \
        10.0.8.0/24 10.0.9.0/24 10.0.10.0/24 10.0.11.0/24 \
        10.0.12.0/24 10.0.13.0/24 10.0.14.0/24 10.0.15.0/24\"\n";
    let (p16, _g16) = create_temp_conf(sixteen_entries);
    let cfg16 = match load(&p16) {
        Ok(c) => c,
        Err(e) => panic!("load 16 failed: {}", e),
    };
    assert_eq!(cfg16.interface.vpn_bypass_route_count, 16);

    // 16 unique entries with duplicate succeeds
    let sixteen_with_dup = "VPN_BYPASS_ROUTES=\"10.0.0.0/24 10.0.1.0/24 10.0.2.0/24 10.0.3.0/24 \
        10.0.4.0/24 10.0.5.0/24 10.0.6.0/24 10.0.7.0/24 \
        10.0.8.0/24 10.0.9.0/24 10.0.10.0/24 10.0.11.0/24 \
        10.0.12.0/24 10.0.13.0/24 10.0.14.0/24 10.0.15.0/24 10.0.0.0/24\"\n";
    let (p16dup, _g16dup) = create_temp_conf(sixteen_with_dup);
    let cfg16dup = match load(&p16dup) {
        Ok(c) => c,
        Err(e) => panic!("load 16 with dup failed: {}", e),
    };
    assert_eq!(cfg16dup.interface.vpn_bypass_route_count, 16);

    // 17 entries rejected
    let seventeen_entries = "VPN_BYPASS_ROUTES=\"10.0.0.0/24 10.0.1.0/24 10.0.2.0/24 10.0.3.0/24 \
        10.0.4.0/24 10.0.5.0/24 10.0.6.0/24 10.0.7.0/24 \
        10.0.8.0/24 10.0.9.0/24 10.0.10.0/24 10.0.11.0/24 \
        10.0.12.0/24 10.0.13.0/24 10.0.14.0/24 10.0.15.0/24 10.0.16.0/24\"\n";
    check_invalid(seventeen_entries);

    // VPN_BYPASS_PREF bounds rejected
    check_invalid("VPN_BYPASS_PREF=0\n");
    check_invalid("VPN_BYPASS_PREF=40000\n");
}

// Mirrors test_vpn_bypass_config.c: Case 7 - Classify-reload check
#[test]
fn test_vpn_bypass_config_classify_reload() {
    let current = Config::default();
    let mut candidate = Config::default();

    let changes = classify_reload(&current, &candidate);
    assert_eq!(changes.changes, ReloadChanges::NONE);

    candidate.interface.vpn_bypass_route_count = 1;
    candidate.interface.vpn_bypass_routes[0].family = AF_INET;
    candidate.interface.vpn_bypass_routes[0].prefix_len = 24;
    let changes = classify_reload(&current, &candidate);
    assert!(changes.contains(ReloadChanges::HOT));
    assert!(!changes.contains(ReloadChanges::REQUIRES_RESTART));

    candidate = current.clone();
    candidate.interface.vpn_bypass_pref = 12000;
    let changes = classify_reload(&current, &candidate);
    assert!(changes.contains(ReloadChanges::HOT));
    assert!(!changes.contains(ReloadChanges::REQUIRES_RESTART));
}

// Mirrors test_vpn_bypass_config.c: Case 8 - Deduplication of identical routes
#[test]
fn test_vpn_bypass_config_deduplication() {
    let (path, _guard) = create_temp_conf(
        "VPN_BYPASS_ROUTES=\"100.64.0.0/10 100.64.0.0/10 192.168.88.1/24 192.168.88.2/24\"\n",
    );
    let cfg = match load(&path) {
        Ok(c) => c,
        Err(e) => panic!("load failed: {}", e),
    };
    assert_eq!(cfg.interface.vpn_bypass_route_count, 2);
}

// Mirrors test_config_value.c: atp_config_t copy and value persistence
#[test]
fn test_config_value_copy() {
    let mut source = Config::default();
    source.service.restart_delay_sec = 2;
    source.api.port = 9080;

    let copy = source.clone();
    assert_eq!(
        copy.service.restart_delay_sec,
        source.service.restart_delay_sec
    );
    assert_eq!(copy.api.port, source.api.port);
}

// Mirrors test_reload_transaction.c: test_field_classification
#[test]
fn test_reload_field_classification() {
    let current = Config::default();

    let expect_restart = |candidate: &Config, field: &str| {
        let res = classify_reload(&current, candidate);
        assert!(
            res.contains(ReloadChanges::REQUIRES_RESTART),
            "expected restart-required for field {}",
            field
        );
        assert!(
            res.changed_fields.contains(field),
            "restart-required field {} missing from: {}",
            field,
            res.changed_fields
        );
    };

    let mut candidate = current.clone();
    candidate.core.data_dir = "/different/data".to_string();
    expect_restart(&candidate, "DATA_DIR");

    candidate = current.clone();
    candidate.core.run_dir = "/different/run".to_string();
    expect_restart(&candidate, "RUN_DIR");

    candidate = current.clone();
    candidate.core.pid_file = "/different/atpd.pid".to_string();
    expect_restart(&candidate, "PID_FILE");

    candidate = current.clone();
    candidate.core.log_file = "/different/atp.log".to_string();
    expect_restart(&candidate, "LOG_FILE");

    candidate = current.clone();
    candidate.core.core_user = "different-user".to_string();
    expect_restart(&candidate, "CORE_USER_GROUP");

    candidate = current.clone();
    candidate.core.core_group = "different-group".to_string();
    expect_restart(&candidate, "CORE_USER_GROUP");

    candidate = current.clone();
    candidate.service.args = "-c different.json".to_string();
    expect_restart(&candidate, "SERVICE_ARGS");

    candidate = current.clone();
    candidate.service.env = "KEY=value".to_string();
    expect_restart(&candidate, "SERVICE_ENV");

    candidate = current.clone();
    candidate.api.host = "127.0.0.2".to_string();
    expect_restart(&candidate, "API_HOST");

    candidate = current.clone();
    candidate.api.port += 1;
    expect_restart(&candidate, "API_PORT");

    candidate = current.clone();
    candidate.api.secret = "different-secret".to_string();
    expect_restart(&candidate, "API_SECRET");

    candidate = current.clone();
    candidate.interface.vpn_auto_mode = !candidate.interface.vpn_auto_mode;
    expect_restart(&candidate, "VPN_AUTO_MODE");

    candidate = current.clone();
    candidate.interface.vpn_target_mode = "Different".to_string();
    expect_restart(&candidate, "VPN_TARGET_MODE");

    candidate = current.clone();
    candidate.interface.vpn_fallback_mode = "Different".to_string();
    expect_restart(&candidate, "VPN_FALLBACK_MODE");

    candidate = current.clone();
    candidate.service.circuit_threshold += 1;
    expect_restart(&candidate, "SERVICE_CIRCUIT_THRESHOLD");

    candidate = current.clone();
    candidate.service.circuit_cooldown_sec += 1;
    expect_restart(&candidate, "SERVICE_CIRCUIT_COOLDOWN");

    // HOT changes
    candidate = current.clone();
    candidate.core.ui_emoji_enabled = !candidate.core.ui_emoji_enabled;
    candidate.service.start_timeout_sec += 1;
    candidate.service.stop_timeout_sec += 1;
    candidate.service.max_failures += 1;
    candidate.service.health_check_interval_ms += 1;
    let res = classify_reload(&current, &candidate);
    assert_eq!(res.changes, ReloadChanges::HOT);
    assert_eq!(res.changed_fields, "");

    // STATIC changes
    candidate = current.clone();
    candidate.core.log_timestamp = !candidate.core.log_timestamp;
    candidate.service.restart_delay_sec += 1;
    candidate.service.grace_period_sec += 1;
    let res = classify_reload(&current, &candidate);
    assert_eq!(res.changes, ReloadChanges::STATIC);
}

// Mirrors test_reload_transaction.c: config_prepare_reload behavior
#[test]
fn test_prepare_reload_behavior() {
    let (path, _guard) = create_temp_conf("API_PORT=9099\n");
    let candidate = match prepare_reload(&path) {
        Ok(c) => c,
        Err(e) => panic!("prepare_reload failed: {}", e),
    };
    assert_eq!(candidate.api.port, 9099);

    // Missing file returns NotFound
    let missing_path = std::env::temp_dir().join("atpd_nonexistent_file.conf");
    let res = prepare_reload(&missing_path);
    assert!(matches!(res, Err(ConfigError::NotFound(_))));

    // Empty path returns InvalidArgument
    let empty_res = prepare_reload("");
    assert!(matches!(empty_res, Err(ConfigError::InvalidArgument(_))));
}

// Mirrors test_config_validation.sh: invalid and valid configuration cases
#[test]
fn test_config_validation_cases() {
    let expect_invalid = |content: &str| {
        let (p, _g) = create_temp_conf(content);
        assert!(
            load(&p).is_err(),
            "expected config to be invalid: {}",
            content
        );
    };

    expect_invalid("API_PORT=0\n");
    expect_invalid("API_P0RT=9080\n");
    expect_invalid("API_PORT=abc\n");
    expect_invalid("SERVICE_START_TIMEOUT=30x\n");
    expect_invalid("VPN_AUTO_MODE=2\n");
    expect_invalid("UI_EMOJI_ENABLED=-1\n");
    expect_invalid("API_PORT=9080\nAPI_PORT=9090\n");

    // 64 'x' for API_HOST
    let x64 = "x".repeat(64);
    expect_invalid(&format!("API_HOST={}\n", x64));

    expect_invalid("API_PORT 9080\n");
    expect_invalid("API_HOST=\"127.0.0.1\n");
    expect_invalid("API_HOST=\n");
    expect_invalid("API_HOST=192.0.2.999\n");
    expect_invalid("API_HOST=localhost\n");
    expect_invalid("API_HOST=::1\n");

    // DATA_DIR with invalid config.json (port: 0)
    let (data_dir, _dir_guard) = create_temp_dir();
    let json_path = data_dir.join("config.json");
    if let Err(e) = std::fs::write(&json_path, r#"{"api":{"port":0}}"#) {
        panic!("failed to write config.json: {}", e);
    }
    expect_invalid(&format!("DATA_DIR={}\n", data_dir.to_string_lossy()));

    // DATA_DIR with invalid config.json (invalid IP in listen)
    if let Err(e) = std::fs::write(
        &json_path,
        r#"{"services":[{"type":"api","listen":"192.0.2.999:9080"}]}"#,
    ) {
        panic!("failed to write config.json: {}", e);
    }
    expect_invalid(&format!("DATA_DIR={}\n", data_dir.to_string_lossy()));

    // Port that wraps to 80 when truncated to i32 must be rejected
    if let Err(e) = std::fs::write(&json_path, r#"{"api":{"port":4294967376}}"#) {
        panic!("failed to write config.json: {}", e);
    }
    expect_invalid(&format!("DATA_DIR={}\n", data_dir.to_string_lossy()));

    // Unreadable config.json (invalid UTF-8) must be rejected, as the C loader does
    if let Err(e) = std::fs::write(&json_path, [0xff, 0xfe]) {
        panic!("failed to write config.json: {}", e);
    }
    expect_invalid(&format!("DATA_DIR={}\n", data_dir.to_string_lossy()));

    // Missing file rejected
    let missing = std::env::temp_dir().join("atpd_missing_test.conf");
    assert!(load(&missing).is_err());

    // Valid configurations
    let (v1, _g1) = create_temp_conf("API_PORT=9080\n");
    let c1 = match load(&v1) {
        Ok(c) => c,
        Err(e) => panic!("valid 1 failed: {}", e),
    };
    assert_eq!(c1.api.port, 9080);

    let (v2, _g2) = create_temp_conf("API_HOST=192.0.2.1\n");
    let c2 = match load(&v2) {
        Ok(c) => c,
        Err(e) => panic!("valid 2 failed: {}", e),
    };
    assert_eq!(c2.api.host, "192.0.2.1");
}

// Additional test for deprecated aliases and config.json sync
#[test]
fn test_deprecated_aliases_and_config_json_sync() {
    // Deprecated alias WORK_DIR
    let (p1, _g1) = create_temp_conf("WORK_DIR=/tmp/custom_data\n");
    let c1 = match load(&p1) {
        Ok(c) => c,
        Err(e) => panic!("alias WORK_DIR failed: {}", e),
    };
    assert_eq!(c1.core.data_dir, "/tmp/custom_data");

    // Duplicate canonical key via alias
    let (p2, _g2) = create_temp_conf("WORK_DIR=/tmp/a\nDATA_DIR=/tmp/b\n");
    assert!(load(&p2).is_err());

    // config.json sync when not present in atp.conf
    let (data_dir, _dir_guard) = create_temp_dir();
    let json_path = data_dir.join("config.json");
    let json_content = r#"{
        "services": [
            {
                "type": "api",
                "listen": "127.0.0.1:9191",
                "secret": "my-secret-key"
            }
        ]
    }"#;
    if let Err(e) = std::fs::write(&json_path, json_content) {
        panic!("failed to write config.json: {}", e);
    }

    let (conf_path, _conf_guard) =
        create_temp_conf(&format!("DATA_DIR={}\n", data_dir.to_string_lossy()));
    let cfg = match load(&conf_path) {
        Ok(c) => c,
        Err(e) => panic!("sync test failed: {}", e),
    };
    assert_eq!(cfg.api.port, 9191);
    assert_eq!(cfg.api.host, "127.0.0.1");
    assert_eq!(cfg.api.secret, "my-secret-key");

    // atp.conf overrides config.json
    let (conf_path2, _conf_guard2) = create_temp_conf(&format!(
        "DATA_DIR={}\nAPI_PORT=9292\n",
        data_dir.to_string_lossy()
    ));
    let cfg2 = match load(&conf_path2) {
        Ok(c) => c,
        Err(e) => panic!("override test failed: {}", e),
    };
    assert_eq!(cfg2.api.port, 9292);
}
