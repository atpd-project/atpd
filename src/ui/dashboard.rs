//! Dashboard human-readable status rendering.
//!
//! Mirrors `src/status_render.c` as pure string builders.
//! Produces byte-identical output to C for the same snapshot and terminal parameters.

use crate::status::StatusSnapshot;
use crate::supervisor::ServiceState;
use crate::ui::{UiRenderCtx, COLOR_GREEN, COLOR_RED, COLOR_YELLOW};

pub fn format_uptime(seconds: i32) -> String {
    if seconds < 0 {
        return "N/A".to_string();
    }
    let days = seconds / 86400;
    let hours = (seconds % 86400) / 3600;
    let mins = (seconds % 3600) / 60;
    let secs = seconds % 60;
    if days > 0 {
        format!("{}d {:02}:{:02}:{:02}", days, hours, mins, secs)
    } else if hours > 0 {
        format!("{}h {:02}m {:02}s", hours, mins, secs)
    } else if mins > 0 {
        format!("{}m {:02}s", mins, secs)
    } else {
        format!("{}s", secs)
    }
}

pub fn format_kb(kb: i64) -> String {
    if kb < 0 {
        "N/A".to_string()
    } else if kb >= 1024 * 1024 {
        format!("{:.2} GB", kb as f64 / (1024.0 * 1024.0))
    } else if kb >= 1024 {
        format!("{:.2} MB", kb as f64 / 1024.0)
    } else {
        format!("{} KB", kb)
    }
}

pub fn format_bytes(bytes: u64) -> String {
    if bytes >= 1024 * 1024 * 1024 {
        format!("{:.2} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    } else if bytes >= 1024 * 1024 {
        format!("{:.2} MB", bytes as f64 / (1024.0 * 1024.0))
    } else if bytes >= 1024 {
        format!("{:.2} KB", bytes as f64 / 1024.0)
    } else {
        format!("{} B", bytes)
    }
}

pub fn format_int(value: i32) -> String {
    if value < 0 {
        "N/A".to_string()
    } else {
        value.to_string()
    }
}

pub fn service_display_state(snapshot: &StatusSnapshot) -> &'static str {
    match snapshot.singbox_state {
        ServiceState::Running => {
            if snapshot.singbox_healthy {
                "RUNNING / healthy"
            } else {
                "RUNNING / unhealthy"
            }
        }
        ServiceState::Starting => "STARTING",
        ServiceState::Failed => "FAILED",
        ServiceState::Stopping => "STOPPING",
        ServiceState::Stopped => "STOPPED",
    }
}

pub fn service_display_color(snapshot: &StatusSnapshot) -> &'static str {
    if snapshot.singbox_state == ServiceState::Running {
        if snapshot.singbox_healthy {
            COLOR_GREEN
        } else {
            COLOR_YELLOW
        }
    } else if snapshot.singbox_state == ServiceState::Starting
        || snapshot.singbox_state == ServiceState::Stopping
    {
        COLOR_YELLOW
    } else {
        COLOR_RED
    }
}

fn render_atpd<W: std::fmt::Write>(
    ui: &mut UiRenderCtx<W>,
    snapshot: &StatusSnapshot,
) -> std::fmt::Result {
    let uptime = format_uptime(snapshot.atpd_uptime_sec);
    let rss = format_kb(snapshot.atpd_rss_kb);
    let hwm = format_kb(snapshot.atpd_hwm_kb);
    let fds = format_int(snapshot.atpd_fd_count);
    let threads = format_int(snapshot.atpd_thread_count);

    ui.table_begin()?;
    ui.table_header("🚀", "ATPD DAEMON")?;
    ui.table_row_color(
        "State",
        if snapshot.daemon_running {
            "RUNNING"
        } else {
            "STOPPED"
        },
        if snapshot.daemon_running {
            COLOR_GREEN
        } else {
            COLOR_YELLOW
        },
    )?;
    if snapshot.daemon_running {
        ui.table_subrow_int("├─", "PID", snapshot.atpd_pid)?;
    }
    ui.table_subrow("├─", "Uptime", &uptime)?;
    ui.table_subrow("├─", "RSS", &rss)?;
    ui.table_subrow("├─", "Peak RSS", &hwm)?;
    ui.table_subrow("├─", "FDs", &fds)?;
    ui.table_subrow("└─", "Threads", &threads)?;
    ui.table_end()?;
    Ok(())
}

fn render_proxy<W: std::fmt::Write>(
    ui: &mut UiRenderCtx<W>,
    snapshot: &StatusSnapshot,
) -> std::fmt::Result {
    ui.table_begin()?;
    ui.table_header("📦", "PROXY CORE")?;
    ui.table_row_color(
        "STATUS",
        service_display_state(snapshot),
        service_display_color(snapshot),
    )?;
    if snapshot.singbox_pid <= 0 {
        ui.table_end()?;
        return Ok(());
    }

    let uptime = format_uptime(snapshot.singbox_uptime_sec);
    let rss = format_kb(snapshot.singbox_rss_kb);
    let hwm = format_kb(snapshot.singbox_hwm_kb);
    let cpu = if snapshot.singbox_cpu_percent < 0.0 {
        "N/A".to_string()
    } else {
        format!("{:.1}%", snapshot.singbox_cpu_percent)
    };
    let threads = format_int(snapshot.singbox_thread_count);
    let fds = format_int(snapshot.singbox_fd_count);
    let goroutines = if snapshot.native_api.valid {
        snapshot.native_api.status.goroutines.to_string()
    } else {
        "N/A".to_string()
    };
    let version = if snapshot.native_api.version_valid {
        snapshot.native_api.version_str()
    } else {
        "N/A"
    };

    ui.table_subrow_int("├─", "PID", snapshot.singbox_pid)?;
    ui.table_subrow("├─", "State", service_display_state(snapshot))?;
    ui.table_subrow("├─", "Uptime", &uptime)?;
    ui.table_subrow("├─", "Memory", &rss)?;
    ui.table_subrow("├─", "Peak Memory", &hwm)?;
    ui.table_subrow("├─", "CPU", &cpu)?;
    ui.table_subrow("├─", "Threads", &threads)?;
    ui.table_subrow("├─", "Goroutines", &goroutines)?;
    ui.table_subrow("├─", "FDs", &fds)?;
    ui.table_subrow("└─", "Version", version)?;
    ui.table_end()?;
    Ok(())
}

fn render_api<W: std::fmt::Write>(
    ui: &mut UiRenderCtx<W>,
    snapshot: &StatusSnapshot,
) -> std::fmt::Result {
    let api = format!("Native API (Port {})", snapshot.api_port);
    let clash_mode = if snapshot.native_api.clash_mode_valid {
        snapshot.native_api.clash_mode_str()
    } else {
        "N/A"
    };
    let clash_color = if snapshot.native_api.clash_mode_valid {
        COLOR_GREEN
    } else {
        COLOR_YELLOW
    };

    ui.table_begin()?;
    ui.table_header("🔌", "NATIVE API & MODE")?;
    ui.table_subrow("├─", "API Engine", &api)?;
    ui.table_subrow_color("└─", "Clash Mode", clash_mode, clash_color)?;
    ui.table_end()?;
    Ok(())
}

fn render_vpn<W: std::fmt::Write>(
    ui: &mut UiRenderCtx<W>,
    snapshot: &StatusSnapshot,
) -> std::fmt::Result {
    ui.table_begin()?;
    ui.table_header("🌐", "VPN TUNNEL STATUS")?;
    let iface = snapshot.vpn.iface_str();
    if snapshot.vpn.state != crate::context::VpnState::Ready || iface.is_empty() {
        ui.table_row_color("State", "STANDALONE / DIRECT", COLOR_GREEN)?;
        ui.table_end()?;
        return Ok(());
    }

    let emoji = ui.emoji_vpn(true);
    ui.table_row_color(emoji, "CONNECTED", COLOR_GREEN)?;
    ui.table_subrow("├─", "Interface", iface)?;
    if snapshot.traffic_available {
        let rx = format_bytes(snapshot.vpn_rx_bytes);
        let tx = format_bytes(snapshot.vpn_tx_bytes);
        ui.table_subrow("├─", "Total RX", &rx)?;
        ui.table_subrow("└─", "Total TX", &tx)?;
    } else {
        ui.table_subrow("├─", "Total RX", "N/A")?;
        ui.table_subrow("└─", "Total TX", "N/A")?;
    }
    ui.table_end()?;
    Ok(())
}

fn render_system<W: std::fmt::Write>(
    ui: &mut UiRenderCtx<W>,
    snapshot: &StatusSnapshot,
) -> std::fmt::Result {
    let temperature = if snapshot.cpu_temperature_c < 0 {
        "N/A".to_string()
    } else {
        format!("{}°C", snapshot.cpu_temperature_c)
    };
    let kernel = if snapshot.kernel_release[0] != 0 {
        snapshot.kernel_release_str()
    } else {
        "N/A"
    };

    ui.table_begin()?;
    ui.table_header("💻", "SYSTEM")?;
    ui.table_subrow("├─", "ATPD Version", crate::version::get_full_version())?;
    ui.table_subrow("├─", "Kernel", kernel)?;
    ui.table_subrow("└─", "CPU Temp", &temperature)?;
    ui.table_end()?;
    Ok(())
}

pub fn status_render_snapshot_width<W: std::fmt::Write>(
    out: W,
    no_color: bool,
    snapshot: &StatusSnapshot,
    width: usize,
) -> std::fmt::Result {
    let mut ui = UiRenderCtx::new(out, width, !no_color, snapshot.emoji_enabled);
    ui.title(if snapshot.emoji_enabled {
        "📊 ATPD Status"
    } else {
        "ATPD Status"
    })?;
    render_atpd(&mut ui, snapshot)?;
    ui.blank()?;
    render_proxy(&mut ui, snapshot)?;
    ui.blank()?;
    render_api(&mut ui, snapshot)?;
    ui.blank()?;
    render_vpn(&mut ui, snapshot)?;
    ui.blank()?;
    render_system(&mut ui, snapshot)?;
    Ok(())
}

pub fn status_render_to_string(snapshot: &StatusSnapshot, width: usize, no_color: bool) -> String {
    let mut buf = String::with_capacity(2048);
    let _ = status_render_snapshot_width(&mut buf, no_color, snapshot, width);
    buf
}

pub fn status_render_summary<W: std::fmt::Write>(
    out: &mut W,
    snapshot: &StatusSnapshot,
) -> std::fmt::Result {
    let atpd_uptime = format_uptime(snapshot.atpd_uptime_sec);
    let atpd_rss = format_kb(snapshot.atpd_rss_kb);
    let singbox_uptime = format_uptime(snapshot.singbox_uptime_sec);
    let singbox_rss = format_kb(snapshot.singbox_rss_kb);

    writeln!(out, "Runtime status:")?;
    write!(
        out,
        "  ATPD:      {}",
        if snapshot.daemon_running {
            "RUNNING"
        } else {
            "STOPPED"
        }
    )?;
    if snapshot.atpd_pid > 0 {
        write!(out, " (PID: {})", snapshot.atpd_pid)?;
    }
    writeln!(out, ", uptime {}, RSS {}", atpd_uptime, atpd_rss)?;

    write!(out, "  sing-box:  {}", service_display_state(snapshot))?;
    if snapshot.singbox_pid > 0 {
        write!(out, " (PID: {})", snapshot.singbox_pid)?;
    }
    writeln!(out, ", uptime {}, memory {}", singbox_uptime, singbox_rss)?;

    let kernel = if snapshot.kernel_release[0] != 0 {
        snapshot.kernel_release_str()
    } else {
        "N/A"
    };
    writeln!(out, "  Kernel:    {}", kernel)?;
    writeln!(out, "  Data path: sing-box ebpf inbound")?;
    Ok(())
}

pub fn status_summary_to_string(snapshot: &StatusSnapshot) -> String {
    let mut buf = String::with_capacity(512);
    let _ = status_render_summary(&mut buf, snapshot);
    buf
}

#[cfg(test)]
mod tests {
    use super::*;

    // Ported from tests/test_status_render.c: render_with_pid
    fn render_with_pid(pid: i32) -> String {
        let mut kernel_release = [0u8; 256];
        let krel = b"6.1.0-test";
        kernel_release[..krel.len()].copy_from_slice(krel);

        let snapshot = StatusSnapshot {
            emoji_enabled: true,
            daemon_running: true,
            api_port: 9080,
            kernel_release,
            atpd_pid: pid,
            atpd_uptime_sec: 5,
            atpd_fd_count: 7,
            atpd_thread_count: 1,
            atpd_rss_kb: 1024,
            atpd_hwm_kb: 1280,
            singbox_pid: -1,
            cpu_temperature_c: -1,
            ..Default::default()
        };

        status_render_to_string(&snapshot, 0, true)
    }

    // Ported from tests/test_status_render.c: render_narrow
    fn render_narrow(emoji_enabled: bool) -> String {
        let mut kernel_release = [0u8; 256];
        let krel = b"6.1.0-test-kernel-release";
        kernel_release[..krel.len()].copy_from_slice(krel);

        let snapshot = StatusSnapshot {
            emoji_enabled,
            daemon_running: true,
            atpd_pid: 10101,
            singbox_state: ServiceState::Running,
            singbox_pid: 30303,
            singbox_healthy: true,
            kernel_release,
            ..Default::default()
        };

        status_render_to_string(&snapshot, 30, true)
    }

    // Ported from tests/test_status_render.c: render_with_native_api
    fn render_with_native_api(traffic_available: bool) -> String {
        let mut version = [0u8; 128];
        let ver = b"1.12.0";
        version[..ver.len()].copy_from_slice(ver);

        let mut clash_mode = [0u8; 64];
        let mode = b"Rule";
        clash_mode[..mode.len()].copy_from_slice(mode);

        let snapshot = StatusSnapshot {
            api_port: 9080,
            singbox_pid: 30303,
            singbox_state: ServiceState::Running,
            singbox_healthy: true,
            singbox_uptime_sec: 10,
            singbox_fd_count: 8,
            singbox_thread_count: 4,
            singbox_rss_kb: 2048,
            singbox_hwm_kb: 4096,
            native_api: crate::status::NativeApiSnapshot {
                valid: true,
                version_valid: true,
                clash_mode_valid: true,
                status: crate::nativeapi::SingboxStatus {
                    goroutines: 17,
                    traffic_available,
                    ..Default::default()
                },
                version,
                clash_mode,
                ..Default::default()
            },
            ..Default::default()
        };

        status_render_to_string(&snapshot, 0, true)
    }

    // Ported from tests/test_status_render.c: render_service_state
    fn render_service_state(state: ServiceState, pid: i32, healthy: bool) -> String {
        let snapshot = StatusSnapshot {
            singbox_state: state,
            singbox_pid: pid,
            singbox_healthy: healthy,
            singbox_uptime_sec: -1,
            singbox_fd_count: -1,
            singbox_thread_count: -1,
            singbox_rss_kb: -1,
            singbox_hwm_kb: -1,
            singbox_cpu_percent: -1.0,
            atpd_uptime_sec: -1,
            atpd_fd_count: -1,
            atpd_thread_count: -1,
            atpd_rss_kb: -1,
            atpd_hwm_kb: -1,
            cpu_temperature_c: -1,
            ..Default::default()
        };

        status_render_to_string(&snapshot, 0, true)
    }

    #[test]
    fn test_status_render_c_golden_mirrored() {
        // Mirrors tests/test_status_render.c main()
        let first = render_with_pid(10101);
        let second = render_with_pid(20202);

        assert!(!first.contains("\x1b["));
        assert!(first.contains("10101"));
        assert!(!first.contains("20202"));
        assert!(second.contains("20202"));
        assert!(!second.contains("10101"));
        assert!(first.contains("Peak RSS"));
        assert!(!first.contains("Data Path"));
        assert!(first.contains("6.1.0-test"));

        let headers = [
            "📊 ATPD Status",
            "🚀 ATPD DAEMON",
            "📦 PROXY CORE",
            "🔌 NATIVE API & MODE",
            "🌐 VPN TUNNEL STATUS",
            "💻 SYSTEM",
        ];
        let narrow_emoji = render_narrow(true);
        let narrow = render_narrow(false);
        for h in &headers {
            assert!(first.contains(h), "first should contain {}", h);
            assert!(
                narrow_emoji.contains(h),
                "narrow_emoji should contain {}",
                h
            );
            assert!(!narrow.contains(h), "narrow should not contain {}", h);
        }
        assert!(narrow.contains("=== ATPD DAEMON ==="));
        assert!(narrow.contains("State: STANDALONE / DIRECT"));
        assert!(narrow_emoji.contains("State: STANDALONE / DIRECT"));
        assert!(!narrow_emoji.contains('ℹ'));
        assert!(!narrow_emoji.contains("\x1b["));
        assert!(narrow.contains("\n    Kernel: "));
        assert!(!narrow.contains("6.1.0-test-kernel-release"));

        let mut kernel_release = [0u8; 256];
        let krel = b"6.1.0-test";
        kernel_release[..krel.len()].copy_from_slice(krel);

        let summary_snapshot = StatusSnapshot {
            daemon_running: true,
            atpd_pid: 10101,
            atpd_uptime_sec: 2,
            atpd_rss_kb: 1024,
            singbox_pid: 30303,
            singbox_state: ServiceState::Running,
            singbox_healthy: true,
            singbox_uptime_sec: 1,
            singbox_rss_kb: 2048,
            kernel_release,
            ..Default::default()
        };

        let summary = status_summary_to_string(&summary_snapshot);
        assert!(summary.contains("ATPD:      RUNNING (PID: 10101)"));
        assert!(summary.contains("sing-box:  RUNNING / healthy (PID: 30303)"));
        assert!(summary.contains("Kernel:    6.1.0-test"));
        assert!(summary.contains("Data path: sing-box ebpf inbound"));

        let active = render_with_native_api(true);
        let standby = render_with_native_api(false);
        assert!(active.contains("Goroutines"));
        assert!(active.contains("17"));
        assert!(active.contains("1.12.0"));
        assert!(active.contains("Rule"));
        assert!(!active.contains("FCM"));
        assert!(!active.contains("MONITORS & SENSING"));
        assert!(!active.contains("Netlink Listener"));
        assert!(!active.contains("XFRM SA Listener"));
        assert_eq!(active, standby);
        assert!(!active.contains("owner snapshot unavailable"));

        let healthy = render_service_state(ServiceState::Running, 40401, true);
        let unhealthy = render_service_state(ServiceState::Running, 40402, false);
        let failed = render_service_state(ServiceState::Failed, 40403, false);
        let starting = render_service_state(ServiceState::Starting, 40404, false);
        assert!(healthy.contains("RUNNING / healthy"));
        assert!(!healthy.contains("RUNNING / unhealthy"));
        assert!(unhealthy.contains("RUNNING / unhealthy"));
        assert!(failed.contains("FAILED"));
        assert!(failed.contains("40403"));
        assert!(starting.contains("STARTING"));
    }
}
