//! IPv6 WebRTC Leak Guard implementation.
//!
//! Prevents proxied applications from learning the device's physical IPv6 address
//! via WebRTC / STUN / ICE default-route socket queries (`getsockname`).
//!
//! Mirrors the policy routing rules and loopback ULA configuration from
//! `AsteriskBOX` (`engine/root/mode/Ipv6LeakGuard.kt`).

use std::fs;
use std::io;
use std::path::Path;

pub const TUN_ROUTING_TABLE: u32 = 2022;
pub const TPROXY_ROUTING_TABLE: u32 = 160;
pub const FIRST_APPLICATION_UID: u32 = 10000;
pub const LOOPBACK_SOURCE_ADDRESS: &str = "fdfe:dcba:9877::1/128";
pub const SKIP_PREF: u32 = 21900;
pub const ROUTE_PREF: u32 = 21901;
pub const NOP_PREF: u32 = 21902;
pub const UNGUARDED_DESTINATIONS: &[&str] = &["fc00::/7", "fe80::/10", "ff00::/8"];
pub const SINGBOX_TUN_AUTO_REDIRECT_OUTPUT_MARK: u32 = 0x200000;

/// Configuration defining the IPv6 leak guard policy rules.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Ipv6LeakGuard {
    pub table: Option<u32>,
    pub core_mark: Option<u32>,
    pub first_uid: u32,
    pub include_uids: Option<Vec<(u32, u32)>>,
    pub exclude_uids: Vec<(u32, u32)>,
    pub loopback_source: bool,
    pub block: bool,
}

/// Helper to merge overlapping or contiguous UID ranges.
pub fn merge_uid_ranges(ranges: &[(u32, u32)]) -> Vec<(u32, u32)> {
    if ranges.is_empty() {
        return Vec::new();
    }
    let mut sorted = ranges.to_vec();
    sorted.sort_by_key(|r| r.0);

    let mut merged = Vec::with_capacity(sorted.len());
    for range in sorted {
        if let Some(last) = merged.last_mut() {
            let last: &mut (u32, u32) = last;
            if range.0 <= last.1.saturating_add(1) {
                last.1 = last.1.max(range.1);
                continue;
            }
        }
        merged.push(range);
    }
    merged
}

/// Parses UID values and range strings from JSON.
pub fn parse_uid_ranges(
    uids: Option<&serde_json::Value>,
    ranges: Option<&serde_json::Value>,
) -> Option<Vec<(u32, u32)>> {
    let mut raw_ranges = Vec::new();

    if let Some(uids_val) = uids {
        match uids_val {
            serde_json::Value::Array(arr) => {
                for v in arr {
                    if let Some(u) = v.as_u64().and_then(|n| u32::try_from(n).ok()) {
                        raw_ranges.push((u, u));
                    } else {
                        crate::log_warn!("IPv6 leak guard: ignoring invalid uid entry {}", v);
                    }
                }
            }
            serde_json::Value::Number(num) => {
                if let Some(u) = num.as_u64().and_then(|n| u32::try_from(n).ok()) {
                    raw_ranges.push((u, u));
                }
            }
            _ => {}
        }
    }

    if let Some(ranges_val) = ranges {
        match ranges_val {
            serde_json::Value::Array(arr) => {
                for v in arr {
                    if let Some(s) = v.as_str() {
                        if let Some(pair) = parse_single_range(s) {
                            raw_ranges.push(pair);
                        } else {
                            crate::log_warn!("IPv6 leak guard: ignoring invalid uid range {:?}", s);
                        }
                    } else {
                        crate::log_warn!("IPv6 leak guard: ignoring invalid uid range {}", v);
                    }
                }
            }
            serde_json::Value::String(s) => {
                if let Some(pair) = parse_single_range(s) {
                    raw_ranges.push(pair);
                }
            }
            _ => {}
        }
    }

    if raw_ranges.is_empty() && uids.is_none() && ranges.is_none() {
        None
    } else {
        Some(merge_uid_ranges(&raw_ranges))
    }
}

fn parse_single_range(s: &str) -> Option<(u32, u32)> {
    let delim = if s.contains(':') {
        ':'
    } else if s.contains('-') {
        '-'
    } else {
        return None;
    };
    let mut parts = s.split(delim);
    let start_str = parts.next()?.trim();
    let end_str = parts.next()?.trim();
    if parts.next().is_some() {
        return None;
    }
    let start: u32 = start_str.parse().ok()?;
    let end: u32 = end_str.parse().ok()?;
    if start <= end {
        Some((start, end))
    } else {
        None
    }
}

/// Inspects sing-box `config.json` content to determine the appropriate `Ipv6LeakGuard`.
pub fn ipv6_leak_guard_from_config(config_json_content: &str) -> Option<Ipv6LeakGuard> {
    let doc: serde_json::Value = serde_json::from_str(config_json_content).ok()?;
    let root = doc.as_object()?;

    let inbounds = root.get("inbounds").and_then(|v| v.as_array())?;

    // 1. Try eBPF inbound
    for inbound in inbounds {
        let obj = match inbound.as_object() {
            Some(o) => o,
            None => continue,
        };
        let itype = obj.get("type").and_then(|v| v.as_str());
        let tag = obj.get("tag").and_then(|v| v.as_str());
        if itype == Some("ebpf") || tag == Some("ebpf-in") {
            let local = obj.get("local").and_then(|v| v.as_object());
            let ipv6 = local
                .and_then(|l| l.get("ipv6"))
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            let cgroup = local
                .and_then(|l| l.get("data_plane"))
                .and_then(|v| v.as_str())
                == Some("cgroup");

            let include_uids = local
                .and_then(|l| parse_uid_ranges(l.get("include_uid"), l.get("include_uid_range")));
            let exclude_uids = local
                .and_then(|l| parse_uid_ranges(l.get("exclude_uid"), l.get("exclude_uid_range")))
                .unwrap_or_default();

            return Some(Ipv6LeakGuard {
                table: None,
                core_mark: None,
                first_uid: FIRST_APPLICATION_UID,
                include_uids: include_uids.filter(|u| !u.is_empty()),
                exclude_uids,
                loopback_source: ipv6 && cgroup,
                block: !ipv6 || !cgroup,
            });
        }
    }

    // 2. Try TUN inbound
    let mut tun_without_ipv6 = false;
    for inbound in inbounds {
        let obj = match inbound.as_object() {
            Some(o) => o,
            None => continue,
        };
        let itype = obj.get("type").and_then(|v| v.as_str());
        let tag = obj.get("tag").and_then(|v| v.as_str());
        if itype == Some("tun") || tag == Some("tun-in") {
            let mut has_ipv6 = false;
            let check_addrs = |val: Option<&serde_json::Value>| -> bool {
                match val {
                    Some(serde_json::Value::Array(arr)) => arr
                        .iter()
                        .any(|v| v.as_str().is_some_and(|s| s.contains(':'))),
                    Some(serde_json::Value::String(s)) => s.contains(':'),
                    _ => false,
                }
            };
            if check_addrs(obj.get("address")) || check_addrs(obj.get("inet6_address")) {
                has_ipv6 = true;
            }

            if !has_ipv6 {
                // IPv4-only tun: no guard unless a tproxy inbound below needs one.
                tun_without_ipv6 = true;
                continue;
            }

            let include_uids =
                parse_uid_ranges(obj.get("include_uid"), obj.get("include_uid_range"));
            let exclude_uids =
                parse_uid_ranges(obj.get("exclude_uid"), obj.get("exclude_uid_range"))
                    .unwrap_or_default();

            return Some(Ipv6LeakGuard {
                table: Some(TUN_ROUTING_TABLE),
                core_mark: Some(SINGBOX_TUN_AUTO_REDIRECT_OUTPUT_MARK),
                first_uid: 0,
                include_uids: include_uids.filter(|u| !u.is_empty()),
                exclude_uids,
                loopback_source: false,
                block: false,
            });
        }
    }

    // 3. Try TPROXY inbound
    for inbound in inbounds {
        let obj = match inbound.as_object() {
            Some(o) => o,
            None => continue,
        };
        let itype = obj.get("type").and_then(|v| v.as_str());
        let tag = obj.get("tag").and_then(|v| v.as_str());
        if itype == Some("tproxy") || tag == Some("tproxy-in") {
            let include_uids =
                parse_uid_ranges(obj.get("include_uid"), obj.get("include_uid_range"));
            let exclude_uids =
                parse_uid_ranges(obj.get("exclude_uid"), obj.get("exclude_uid_range"))
                    .unwrap_or_default();

            return Some(Ipv6LeakGuard {
                table: Some(TPROXY_ROUTING_TABLE),
                core_mark: None,
                first_uid: FIRST_APPLICATION_UID,
                include_uids: include_uids.filter(|u| !u.is_empty()),
                exclude_uids,
                loopback_source: true,
                block: false,
            });
        }
    }

    if tun_without_ipv6 {
        return None;
    }

    // Default fallback: eBPF socket_assign mode (block public IPv6 default-route lookup)
    Some(Ipv6LeakGuard {
        table: None,
        core_mark: None,
        first_uid: FIRST_APPLICATION_UID,
        include_uids: None,
        exclude_uids: Vec::new(),
        loopback_source: false,
        block: true,
    })
}

/// Generates the shell commands needed to configure policy routing rules and lo address.
pub fn ipv6_leak_guard_commands(guard: Option<&Ipv6LeakGuard>) -> Vec<String> {
    let mut cmds = Vec::new();
    for pref in [SKIP_PREF, ROUTE_PREF, NOP_PREF] {
        cmds.push(format!(
            "while ip -6 rule del pref {} 2>/dev/null; do :; done",
            pref
        ));
    }
    cmds.push(format!(
        "ip -6 addr del {} dev lo 2>/dev/null || true",
        LOOPBACK_SOURCE_ADDRESS
    ));

    let guard = match guard {
        Some(g) => g,
        None => return cmds,
    };

    if guard.loopback_source {
        cmds.push(format!("ip -6 addr add {} dev lo", LOOPBACK_SOURCE_ADDRESS));
    }

    if guard.table.is_none() && !guard.block {
        return cmds;
    }

    let includes: Option<Vec<(u32, u32)>> = guard.include_uids.as_ref().map(|ranges| {
        ranges
            .iter()
            .filter_map(|&(start, end)| {
                let first = start.max(guard.first_uid);
                if first <= end {
                    Some((first, end))
                } else {
                    None
                }
            })
            .collect()
    });

    if let Some(ref inc) = includes {
        if inc.is_empty() {
            return cmds;
        }
    }

    let skip = format!("ip -6 rule add pref {}", SKIP_PREF);
    let route = format!("ip -6 rule add pref {}", ROUTE_PREF);

    cmds.push(format!("ip -6 rule add pref {} nop", NOP_PREF));
    cmds.push(format!("{} not iif lo goto {}", skip, NOP_PREF));

    if let Some(core_mark) = guard.core_mark {
        cmds.push(format!(
            "{} fwmark 0x{:x}/0x{:x} goto {}",
            skip, core_mark, core_mark, NOP_PREF
        ));
    }

    for dst in UNGUARDED_DESTINATIONS {
        cmds.push(format!("{} to {} goto {}", skip, dst, NOP_PREF));
    }

    if guard.first_uid > 0 {
        cmds.push(format!(
            "{} uidrange 0-{} goto {}",
            skip,
            guard.first_uid.saturating_sub(1),
            NOP_PREF
        ));
    }

    for &(start, end) in &guard.exclude_uids {
        cmds.push(format!(
            "{} uidrange {}-{} goto {}",
            skip, start, end, NOP_PREF
        ));
    }

    let selectors: Vec<String> = match includes {
        Some(ref inc) => inc
            .iter()
            .map(|&(s, e)| format!("uidrange {}-{}", s, e))
            .collect(),
        None => Vec::new(),
    };

    if let Some(table) = guard.table {
        if selectors.is_empty() {
            cmds.push(format!("{} lookup {}", route, table));
        } else {
            for sel in &selectors {
                cmds.push(format!("{} {} lookup {}", route, sel, table));
            }
        }
    }

    if guard.block {
        if selectors.is_empty() {
            cmds.push(format!("{} unreachable", route));
        } else {
            for sel in &selectors {
                cmds.push(format!("{} {} unreachable", route, sel));
            }
        }
    }

    cmds
}

/// Executes shell commands to apply IPv6 leak guard rules.
///
/// Runs under `set -e`: the cleanup prefix cannot fail, so the first failing
/// `ip ... add` aborts the script and is reported as an error.
pub fn exec_leak_guard_commands(commands: &[String]) -> io::Result<()> {
    if commands.is_empty() {
        return Ok(());
    }
    let script = format!("set -e\n{}", commands.join("\n"));
    let output = run_sh(&script)?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "IPv6 leak guard command failed ({:?}): {}",
            output.status.code(),
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    Ok(())
}

fn run_sh(script: &str) -> io::Result<std::process::Output> {
    std::process::Command::new("/system/bin/sh")
        .arg("-c")
        .arg(script)
        .output()
        .or_else(|_| {
            std::process::Command::new("sh")
                .arg("-c")
                .arg(script)
                .output()
        })
}

/// Applies IPv6 leak guard rules for the given configuration.
pub fn apply_ipv6_leak_guard(guard: Option<&Ipv6LeakGuard>) -> io::Result<()> {
    let cmds = ipv6_leak_guard_commands(guard);
    exec_leak_guard_commands(&cmds)
}

/// Reads the config and derives the desired guard. Any read/parse failure is an
/// error so callers keep the existing rules (fail closed) instead of removing them.
fn guard_from_config_file(conf_path: &str) -> io::Result<Option<Ipv6LeakGuard>> {
    let content = fs::read_to_string(Path::new(conf_path))?;
    serde_json::from_str::<serde_json::Value>(&content)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    Ok(ipv6_leak_guard_from_config(&content))
}

/// Reads sing-box config file and applies IPv6 leak guard.
/// On read/parse errors the existing rules are left untouched.
pub fn apply_ipv6_leak_guard_from_config_file(conf_path: &str) -> io::Result<()> {
    let guard = guard_from_config_file(conf_path)?;
    crate::log_info!(
        "IPv6 leak guard: applying rules (active={})",
        guard.is_some()
    );
    apply_ipv6_leak_guard(guard.as_ref())
}

/// Re-applies the guard only if its rules were flushed (e.g. by netd); the rule
/// multicast groups are not subscribed to, so this runs from the netlink debounce.
pub fn reconcile_ipv6_leak_guard(conf_path: &str) -> io::Result<()> {
    let guard = guard_from_config_file(conf_path)?;
    let cmds = ipv6_leak_guard_commands(guard.as_ref());
    let route_add = format!("rule add pref {}", ROUTE_PREF);
    if !cmds.iter().any(|c| c.contains(&route_add)) {
        return Ok(());
    }
    let out = run_sh("ip -6 rule show")?;
    let shown = String::from_utf8_lossy(&out.stdout);
    let has = |pref: u32| {
        let prefix = format!("{}:", pref);
        shown.lines().any(|l| l.starts_with(&prefix))
    };
    if out.status.success() && has(SKIP_PREF) && has(ROUTE_PREF) && has(NOP_PREF) {
        return Ok(());
    }
    crate::log_warn!("IPv6 leak guard: rules missing, re-applying");
    exec_leak_guard_commands(&cmds)
}

/// Cleans up any existing IPv6 leak guard rules and lo address.
pub fn cleanup_ipv6_leak_guard() -> io::Result<()> {
    crate::log_info!("IPv6 leak guard: cleaning up rules");
    apply_ipv6_leak_guard(None)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_merge_uid_ranges() {
        assert_eq!(merge_uid_ranges(&[]), Vec::<(u32, u32)>::new());
        assert_eq!(merge_uid_ranges(&[(10000, 10000)]), vec![(10000, 10000)]);
        // Overlapping
        assert_eq!(
            merge_uid_ranges(&[(10000, 10010), (10005, 10020)]),
            vec![(10000, 10020)]
        );
        // Contiguous
        assert_eq!(
            merge_uid_ranges(&[(10000, 10005), (10006, 10010)]),
            vec![(10000, 10010)]
        );
        // Disjoint
        assert_eq!(
            merge_uid_ranges(&[(10000, 10005), (10010, 10020)]),
            vec![(10000, 10005), (10010, 10020)]
        );
        // Unsorted input
        assert_eq!(
            merge_uid_ranges(&[(10020, 10030), (10000, 10010), (10005, 10015)]),
            vec![(10000, 10015), (10020, 10030)]
        );
    }

    #[test]
    fn test_parse_uid_ranges() {
        let uids = serde_json::json!([10001, 10002, 10005]);
        let ranges = serde_json::json!(["10010:10020", "10015:10030", "10040-10050"]);
        let parsed = parse_uid_ranges(Some(&uids), Some(&ranges)).unwrap();
        assert_eq!(
            parsed,
            vec![
                (10001, 10002),
                (10005, 10005),
                (10010, 10030),
                (10040, 10050)
            ]
        );

        // None inputs
        assert_eq!(parse_uid_ranges(None, None), None);
    }

    #[test]
    fn test_ebpf_socket_assign_commands() {
        let json = r#"{
            "inbounds": [{
                "type": "ebpf",
                "tag": "ebpf-in",
                "local": {
                    "ipv6": false,
                    "data_plane": "socket_assign",
                    "exclude_uid": [10050]
                }
            }]
        }"#;
        let guard = ipv6_leak_guard_from_config(json).unwrap();
        assert_eq!(guard.first_uid, FIRST_APPLICATION_UID);
        assert!(guard.block);
        assert!(!guard.loopback_source);
        assert_eq!(guard.exclude_uids, vec![(10050, 10050)]);

        let cmds = ipv6_leak_guard_commands(Some(&guard));
        assert!(cmds
            .iter()
            .any(|c| c.contains("while ip -6 rule del pref 21900")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("ip -6 rule add pref 21902 nop")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 not iif lo goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 to fc00::/7 goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 to fe80::/10 goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 to ff00::/8 goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 uidrange 0-9999 goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 uidrange 10050-10050 goto 21902")));
        assert!(cmds.iter().any(|c| c.contains("pref 21901 unreachable")));
        assert!(!cmds.iter().any(|c| c.contains("ip -6 addr add")));
    }

    #[test]
    fn test_ebpf_cgroup_commands() {
        let json = r#"{
            "inbounds": [{
                "type": "ebpf",
                "tag": "ebpf-in",
                "local": {
                    "ipv6": true,
                    "data_plane": "cgroup"
                }
            }]
        }"#;
        let guard = ipv6_leak_guard_from_config(json).unwrap();
        assert!(!guard.block);
        assert!(guard.loopback_source);
        assert_eq!(guard.table, None);

        let cmds = ipv6_leak_guard_commands(Some(&guard));
        assert!(cmds
            .iter()
            .any(|c| c.contains("ip -6 addr add fdfe:dcba:9877::1/128 dev lo")));
        // In cgroup mode without table/block, no routing rules are needed beyond cleanup & lo address
        assert!(!cmds.iter().any(|c| c.contains("pref 21901 unreachable")));
    }

    #[test]
    fn test_tun_mode_commands() {
        let json = r#"{
            "inbounds": [{
                "type": "tun",
                "tag": "tun-in",
                "address": ["172.19.0.1/30", "fdfe:dcba:9876::1/126"],
                "include_uid": [10100],
                "exclude_uid": [10200]
            }]
        }"#;
        let guard = ipv6_leak_guard_from_config(json).unwrap();
        assert_eq!(guard.table, Some(TUN_ROUTING_TABLE));
        assert_eq!(guard.core_mark, Some(SINGBOX_TUN_AUTO_REDIRECT_OUTPUT_MARK));
        assert_eq!(guard.first_uid, 0);

        let cmds = ipv6_leak_guard_commands(Some(&guard));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 fwmark 0x200000/0x200000 goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21900 uidrange 10200-10200 goto 21902")));
        assert!(cmds
            .iter()
            .any(|c| c.contains("pref 21901 uidrange 10100-10100 lookup 2022")));
    }

    #[test]
    fn test_cleanup_commands() {
        let cmds = ipv6_leak_guard_commands(None);
        assert_eq!(cmds.len(), 4);
        assert!(cmds[0].contains("pref 21900"));
        assert!(cmds[1].contains("pref 21901"));
        assert!(cmds[2].contains("pref 21902"));
        assert!(cmds[3].contains("ip -6 addr del fdfe:dcba:9877::1/128 dev lo"));
    }

    #[test]
    fn test_ipv4_tun_then_tproxy_uses_tproxy() {
        let json = r#"{"inbounds":[
            {"type":"tun","address":["172.19.0.1/30"]},
            {"type":"tproxy"}]}"#;
        assert_eq!(
            ipv6_leak_guard_from_config(json).unwrap().table,
            Some(TPROXY_ROUTING_TABLE)
        );
        let json = r#"{"inbounds":[{"type":"tun","address":["172.19.0.1/30"]}]}"#;
        assert_eq!(ipv6_leak_guard_from_config(json), None);
    }

    #[test]
    fn test_exec_reports_failure_and_bad_config_keeps_rules() {
        assert!(exec_leak_guard_commands(&["false".to_string()]).is_err());
        assert!(exec_leak_guard_commands(&["true".to_string()]).is_ok());
        assert!(apply_ipv6_leak_guard_from_config_file("/nonexistent/config.json").is_err());
    }
}
