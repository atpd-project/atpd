//! Status snapshot collection and process inspection.
//!
//! Mirrors `src/status.c`, `include/status.h`, and process helpers from `src/utils.c`.
//! Reads only `/proc` and sysfs; never touches netlink sockets or the network.

use std::sync::Mutex;

pub type SupervisorSnapshot = crate::supervisor::ServiceSnapshot;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeApiSnapshot {
    pub valid: bool,
    pub version_valid: bool,
    pub clash_mode_valid: bool,
    pub generation: u64,
    pub updated_at_ms: u64,
    pub status: crate::nativeapi::SingboxStatus,
    pub version: [u8; 128],
    pub clash_mode: [u8; 64],
}

impl Default for NativeApiSnapshot {
    fn default() -> Self {
        Self {
            valid: false,
            version_valid: false,
            clash_mode_valid: false,
            generation: 0,
            updated_at_ms: 0,
            status: crate::nativeapi::SingboxStatus::default(),
            version: [0; 128],
            clash_mode: [0; 64],
        }
    }
}

impl NativeApiSnapshot {
    pub fn from_api_snapshot(snap: &crate::nativeapi::ApiSnapshot) -> Self {
        let mut version = [0u8; 128];
        let vbytes = snap.version.as_bytes();
        let vlen = vbytes.len().min(127);
        version[..vlen].copy_from_slice(&vbytes[..vlen]);

        let mut clash_mode = [0u8; 64];
        let cbytes = snap.clash_mode.as_bytes();
        let clen = cbytes.len().min(63);
        clash_mode[..clen].copy_from_slice(&cbytes[..clen]);

        Self {
            valid: snap.valid,
            version_valid: snap.version_valid,
            clash_mode_valid: snap.clash_mode_valid,
            generation: snap.generation,
            updated_at_ms: snap.updated_at_ms,
            status: snap.status,
            version,
            clash_mode,
        }
    }

    pub fn version_str(&self) -> &str {
        let len = self
            .version
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.version.len());
        std::str::from_utf8(&self.version[..len]).unwrap_or("")
    }

    pub fn clash_mode_str(&self) -> &str {
        let len = self
            .clash_mode
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.clash_mode.len());
        std::str::from_utf8(&self.clash_mode[..len]).unwrap_or("")
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StatusSnapshot {
    pub collected_at_ms: u64,
    pub emoji_enabled: bool,
    pub daemon_running: bool,
    pub api_port: i32,
    pub kernel_release: [u8; 256],

    pub atpd_pid: i32,
    pub atpd_uptime_sec: i32,
    pub atpd_fd_count: i32,
    pub atpd_thread_count: i32,
    pub atpd_rss_kb: i64,
    pub atpd_hwm_kb: i64,

    pub singbox_pid: i32,
    pub singbox_state: crate::supervisor::ServiceState,
    pub singbox_healthy: bool,
    pub native_api: NativeApiSnapshot,
    pub singbox_uptime_sec: i32,
    pub singbox_fd_count: i32,
    pub singbox_thread_count: i32,
    pub singbox_rss_kb: i64,
    pub singbox_hwm_kb: i64,
    pub singbox_cpu_percent: f64,

    pub vpn: crate::context::VpnSnapshot,
    pub traffic_available: bool,
    pub vpn_rx_bytes: u64,
    pub vpn_tx_bytes: u64,
    pub cpu_temperature_c: i32,
}

impl Default for StatusSnapshot {
    fn default() -> Self {
        Self {
            collected_at_ms: 0,
            emoji_enabled: true,
            daemon_running: false,
            api_port: crate::config::DEFAULT_API_PORT,
            kernel_release: [0; 256],
            atpd_pid: -1,
            atpd_uptime_sec: -1,
            atpd_fd_count: -1,
            atpd_thread_count: -1,
            atpd_rss_kb: -1,
            atpd_hwm_kb: -1,
            singbox_pid: -1,
            singbox_state: crate::supervisor::ServiceState::Stopped,
            singbox_healthy: false,
            native_api: NativeApiSnapshot::default(),
            singbox_uptime_sec: -1,
            singbox_fd_count: -1,
            singbox_thread_count: -1,
            singbox_rss_kb: -1,
            singbox_hwm_kb: -1,
            singbox_cpu_percent: -1.0,
            vpn: crate::context::VpnSnapshot {
                state: crate::context::VpnState::Idle,
                if_id: 0,
                iface: [0; 32],
                changed_at: libc::timespec {
                    tv_sec: 0,
                    tv_nsec: 0,
                },
                transitions: 0,
            },
            traffic_available: false,
            vpn_rx_bytes: 0,
            vpn_tx_bytes: 0,
            cpu_temperature_c: -1,
        }
    }
}

impl StatusSnapshot {
    pub fn kernel_release_str(&self) -> &str {
        let len = self
            .kernel_release
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.kernel_release.len());
        std::str::from_utf8(&self.kernel_release[..len]).unwrap_or("")
    }
}

struct CpuCacheEntry {
    pid: i32,
    starttime_ticks: u64,
    last_total: f64,
    last_time_ms: u64,
}

static CPU_CACHE: Mutex<Vec<CpuCacheEntry>> = Mutex::new(Vec::new());

fn read_proc_stat(pid: i32) -> Option<(u64, u64, u64)> {
    if pid <= 0 {
        return None;
    }
    let content = std::fs::read_to_string(format!("/proc/{}/stat", pid)).ok()?;
    let close_paren = content.rfind(')')?;
    let mut fields = content[close_paren + 1..].split_ascii_whitespace();
    let _state = fields.next()?;
    for _ in 4..14 {
        fields.next()?;
    }
    let utime = fields.next()?.parse::<u64>().ok()?;
    let stime = fields.next()?.parse::<u64>().ok()?;
    for _ in 16..22 {
        fields.next()?;
    }
    let starttime = fields.next()?.parse::<u64>().ok()?;
    Some((utime, stime, starttime))
}

pub fn get_process_cpu_percent(pid: i32) -> f64 {
    // SAFETY: sysconf with _SC_CLK_TCK is a valid POSIX query.
    let ticks_per_sec = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if ticks_per_sec <= 0 {
        return 0.0;
    }
    let (utime, stime, starttime) = match read_proc_stat(pid) {
        Some(vals) => vals,
        None => return 0.0,
    };
    let total_time = (utime + stime) as f64 / (ticks_per_sec as f64);
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: ts is a valid pointer to stack-allocated timespec.
    let _ = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    let now = (ts.tv_sec as u64) * 1000 + (ts.tv_nsec as u64) / 1_000_000;

    let mut cache_guard = match CPU_CACHE.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    };
    if let Some(entry) = cache_guard.iter_mut().find(|e| e.pid == pid) {
        if entry.starttime_ticks != starttime {
            entry.starttime_ticks = starttime;
            entry.last_total = total_time;
            entry.last_time_ms = now;
            return 0.0;
        }
        let mut cpu_percent = 0.0;
        if entry.last_time_ms > 0 && now > entry.last_time_ms {
            let elapsed = (now - entry.last_time_ms) as f64 / 1000.0;
            if elapsed > 0.0 {
                cpu_percent =
                    (((total_time - entry.last_total) / elapsed) * 100.0).clamp(0.0, 100.0);
            }
        }
        entry.last_total = total_time;
        entry.last_time_ms = now;
        cpu_percent
    } else {
        cache_guard.push(CpuCacheEntry {
            pid,
            starttime_ticks: starttime,
            last_total: total_time,
            last_time_ms: now,
        });
        0.0
    }
}

pub fn get_process_uptime_sec(pid: i32) -> i32 {
    // SAFETY: sysconf with _SC_CLK_TCK is a valid POSIX query.
    let ticks_per_sec = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if ticks_per_sec <= 0 {
        return 0;
    }
    let start_time = match read_proc_stat(pid) {
        Some((_, _, starttime)) if starttime > 0 => starttime,
        _ => return 0,
    };
    let uptime_content = match std::fs::read_to_string("/proc/uptime") {
        Ok(c) => c,
        Err(_) => return 0,
    };
    let uptime_sec = match uptime_content
        .split_ascii_whitespace()
        .next()
        .and_then(|s| s.parse::<f64>().ok())
    {
        Some(sec) => sec,
        None => return 0,
    };
    let process_start = start_time as f64 / ticks_per_sec as f64;
    let elapsed = (uptime_sec - process_start) as i32;
    elapsed.max(0)
}

pub fn get_process_fd_count(pid: i32) -> i32 {
    if pid <= 0 {
        return 0;
    }
    let entries = match std::fs::read_dir(format!("/proc/{}/fd", pid)) {
        Ok(entries) => entries,
        Err(_) => return 0,
    };
    let mut count = 0;
    for entry in entries.flatten() {
        if let Ok(name) = entry.file_name().into_string() {
            if !name.starts_with('.') {
                count += 1;
            }
        }
    }
    count
}

pub fn get_process_threads(pid: i32) -> i32 {
    if pid <= 0 {
        return 0;
    }
    let content = match std::fs::read_to_string(format!("/proc/{}/status", pid)) {
        Ok(c) => c,
        Err(_) => return 0,
    };
    for line in content.lines() {
        if let Some(rest) = line.strip_prefix("Threads:") {
            if let Some(num_str) = rest.split_ascii_whitespace().next() {
                if let Ok(val) = num_str.parse::<i32>() {
                    return val;
                }
            }
        }
    }
    0
}

pub fn read_process_memory(pid: i32) -> (i64, i64) {
    let mut rss_kb = -1i64;
    let mut hwm_kb = -1i64;
    if pid <= 0 {
        return (rss_kb, hwm_kb);
    }
    let content = match std::fs::read_to_string(format!("/proc/{}/status", pid)) {
        Ok(c) => c,
        Err(_) => return (rss_kb, hwm_kb),
    };
    for line in content.lines() {
        if let Some(rest) = line.strip_prefix("VmRSS:") {
            if let Some(num_str) = rest.split_ascii_whitespace().next() {
                if let Ok(val) = num_str.parse::<i64>() {
                    rss_kb = val;
                }
            }
        } else if let Some(rest) = line.strip_prefix("VmHWM:") {
            if let Some(num_str) = rest.split_ascii_whitespace().next() {
                if let Ok(val) = num_str.parse::<i64>() {
                    hwm_kb = val;
                }
            }
        }
    }
    (rss_kb, hwm_kb)
}

pub fn read_cpu_temperature() -> i32 {
    const THERMAL_ZONE_BASE: &str = "/sys/class/thermal";
    let entries = match std::fs::read_dir(THERMAL_ZONE_BASE) {
        Ok(e) => e,
        Err(_) => return -1,
    };
    for entry in entries.flatten() {
        let file_name = entry.file_name();
        let name = file_name.to_string_lossy();
        if name.starts_with("thermal_zone") {
            let temp_path = format!("{}/{}/temp", THERMAL_ZONE_BASE, name);
            if let Ok(text) = std::fs::read_to_string(temp_path) {
                if let Some(first_word) = text.split_ascii_whitespace().next() {
                    if let Ok(val) = first_word.parse::<i64>() {
                        if val > 0 {
                            return if val > 1000 {
                                (val / 1000) as i32
                            } else {
                                val as i32
                            };
                        }
                    }
                }
            }
        }
    }
    -1
}

pub fn read_interface_traffic(iface: &str) -> Option<(u64, u64)> {
    if iface.is_empty() {
        return None;
    }
    let content = std::fs::read_to_string("/proc/net/dev").ok()?;
    for line in content.lines() {
        if let Some((name, rx, tx)) = crate::netlink::stats::parse_proc_net_dev_line(line) {
            if name == iface {
                return Some((rx, tx));
            }
        }
    }
    None
}

pub fn collect(
    cfg: &crate::config::Config,
    supervisor: &SupervisorSnapshot,
    api: &crate::nativeapi::ApiSnapshot,
    vpn: &crate::context::VpnSnapshot,
) -> StatusSnapshot {
    let mut out = StatusSnapshot {
        collected_at_ms: std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |d| d.as_millis() as u64),
        emoji_enabled: cfg.core.ui_emoji_enabled,
        api_port: if cfg.api.port > 0 {
            cfg.api.port
        } else {
            crate::config::DEFAULT_API_PORT
        },
        ..Default::default()
    };

    let mut system_info = std::mem::MaybeUninit::<libc::utsname>::uninit();
    // SAFETY: system_info is a valid pointer to uninitialized utsname.
    let uname_ret = unsafe { libc::uname(system_info.as_mut_ptr()) };
    if uname_ret == 0 {
        // SAFETY: uname returned 0, guaranteeing system_info was initialized.
        let info = unsafe { system_info.assume_init() };
        // SAFETY: uname guarantees info.release is a null-terminated C string.
        let rel_bytes = unsafe { std::ffi::CStr::from_ptr(info.release.as_ptr()).to_bytes() };
        let len = rel_bytes.len().min(out.kernel_release.len() - 1);
        out.kernel_release[..len].copy_from_slice(&rel_bytes[..len]);
    }

    out.daemon_running = supervisor.state != crate::supervisor::ServiceState::Stopped
        || supervisor.child_pid.is_some()
        || supervisor.start_time > 0;
    if out.daemon_running {
        // SAFETY: getpid is a reentrant syscall that cannot fail.
        let pid = unsafe { libc::getpid() };
        out.atpd_pid = pid;
        out.atpd_uptime_sec = get_process_uptime_sec(pid);
        out.atpd_fd_count = get_process_fd_count(pid);
        out.atpd_thread_count = get_process_threads(pid);
        let (rss, hwm) = read_process_memory(pid);
        out.atpd_rss_kb = rss;
        out.atpd_hwm_kb = hwm;
    }

    out.singbox_pid = supervisor.child_pid.unwrap_or(-1);
    out.singbox_state = supervisor.state;
    out.singbox_healthy = supervisor.healthy;
    out.native_api = NativeApiSnapshot::from_api_snapshot(api);

    if out.singbox_pid > 0 {
        out.singbox_uptime_sec = get_process_uptime_sec(out.singbox_pid);
        out.singbox_fd_count = get_process_fd_count(out.singbox_pid);
        out.singbox_thread_count = get_process_threads(out.singbox_pid);
        out.singbox_cpu_percent = get_process_cpu_percent(out.singbox_pid);
        let (rss, hwm) = read_process_memory(out.singbox_pid);
        out.singbox_rss_kb = rss;
        out.singbox_hwm_kb = hwm;
    }

    out.vpn = *vpn;
    let iface = out.vpn.iface_str();
    if !iface.is_empty() {
        if let Some((rx, tx)) = read_interface_traffic(iface) {
            out.traffic_available = true;
            out.vpn_rx_bytes = rx;
            out.vpn_tx_bytes = tx;
        }
    }

    out.cpu_temperature_c = read_cpu_temperature();
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    // Ported from tests/test_api_snapshot.c and status inspection logic
    #[test]
    fn test_status_collect_proc_and_sysfs() {
        let cfg = crate::config::Config::default();
        // SAFETY: getpid syscall in test.
        let my_pid = unsafe { libc::getpid() };
        let supervisor = SupervisorSnapshot {
            child_pid: Some(my_pid),
            state: crate::supervisor::ServiceState::Running,
            healthy: true,
            start_time: 100,
        };
        let api = crate::nativeapi::ApiSnapshot {
            valid: true,
            version_valid: true,
            clash_mode_valid: true,
            version: "1.12.0".to_string(),
            clash_mode: "Rule".to_string(),
            status: crate::nativeapi::SingboxStatus {
                goroutines: 42,
                ..Default::default()
            },
            ..Default::default()
        };

        let vpn = crate::context::VpnSnapshot {
            state: crate::context::VpnState::Idle,
            if_id: 0,
            iface: [0; 32],
            changed_at: libc::timespec {
                tv_sec: 0,
                tv_nsec: 0,
            },
            transitions: 0,
        };

        let snapshot = collect(&cfg, &supervisor, &api, &vpn);

        assert!(snapshot.daemon_running);
        assert_eq!(snapshot.atpd_pid, my_pid);
        assert!(snapshot.atpd_uptime_sec >= 0);
        assert!(snapshot.atpd_fd_count > 0);
        assert!(snapshot.atpd_thread_count > 0);
        assert!(!snapshot.kernel_release_str().is_empty());

        assert_eq!(snapshot.singbox_pid, my_pid);
        assert_eq!(
            snapshot.singbox_state,
            crate::supervisor::ServiceState::Running
        );
        assert!(snapshot.singbox_healthy);
        assert!(snapshot.native_api.valid);
        assert_eq!(snapshot.native_api.version_str(), "1.12.0");
        assert_eq!(snapshot.native_api.clash_mode_str(), "Rule");
        assert_eq!(snapshot.native_api.status.goroutines, 42);
        assert_eq!(snapshot.singbox_fd_count, snapshot.atpd_fd_count);
    }

    #[test]
    fn test_status_helpers_robust() {
        assert_eq!(get_process_uptime_sec(-1), 0);
        assert_eq!(get_process_fd_count(-1), 0);
        assert_eq!(get_process_threads(-1), 0);
        assert_eq!(get_process_cpu_percent(-1), 0.0);
        assert_eq!(read_process_memory(-1), (-1, -1));
        assert!(read_interface_traffic("").is_none());
    }
}
