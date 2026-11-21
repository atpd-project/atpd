use std::fs;
use std::io;

pub const PROC_TCP_CONG: &str = "/proc/sys/net/ipv4/tcp_congestion_control";
pub const PROC_TCP_AVAIL: &str = "/proc/sys/net/ipv4/tcp_available_congestion_control";
pub const PROC_DEFAULT_QDISC: &str = "/proc/sys/net/core/default_qdisc";

/// Selects the target congestion control algorithm based on requested mode and available algorithms.
pub fn select_congestion_target<'a>(requested: &str, available: &'a [String]) -> Option<&'a str> {
    let req = requested.trim();
    if req.eq_ignore_ascii_case("off")
        || req.eq_ignore_ascii_case("none")
        || req.eq_ignore_ascii_case("disabled")
    {
        return None;
    }

    if req.is_empty() || req.eq_ignore_ascii_case("auto") {
        if let Some(m) = available.iter().find(|&a| a == "bbr3") {
            Some(m.as_str())
        } else if let Some(m) = available.iter().find(|&a| a == "bbr") {
            Some(m.as_str())
        } else {
            None
        }
    } else {
        available
            .iter()
            .find(|&a| a.eq_ignore_ascii_case(req))
            .map(|s| s.as_str())
    }
}

/// Reads the currently active TCP congestion control algorithm.
pub fn read_current_congestion() -> io::Result<String> {
    fs::read_to_string(PROC_TCP_CONG).map(|s| s.trim().to_string())
}

/// Reads the available TCP congestion control algorithms from the kernel.
pub fn read_available_congestion() -> io::Result<Vec<String>> {
    let content = fs::read_to_string(PROC_TCP_AVAIL)?;
    Ok(content
        .split_ascii_whitespace()
        .map(|s| s.to_string())
        .collect())
}

/// Reads the default qdisc from the kernel.
pub fn read_default_qdisc() -> io::Result<String> {
    fs::read_to_string(PROC_DEFAULT_QDISC).map(|s| s.trim().to_string())
}

/// Applies the requested TCP congestion control setting and ensures FQ qdisc for BBR/BBRv3.
pub fn apply_tcp_congestion(requested: &str) -> io::Result<Option<String>> {
    let available = match read_available_congestion() {
        Ok(avail) => avail,
        Err(e) => {
            crate::log_debug!("Failed to read available TCP congestion algorithms: {}", e);
            return Ok(None);
        }
    };

    let target = match select_congestion_target(requested, &available) {
        Some(t) => t,
        None => return Ok(None),
    };

    let current = read_current_congestion().unwrap_or_default();
    if current != target {
        if let Err(e) = fs::write(PROC_TCP_CONG, target) {
            crate::log_warn!(
                "Failed to set TCP congestion control to '{}': {}",
                target,
                e
            );
            return Ok(None);
        }
        crate::log_info!(
            "TCP congestion control set to '{}' (was '{}')",
            target,
            current
        );
    }

    if target == "bbr3" || target == "bbr" {
        let current_qdisc = read_default_qdisc().unwrap_or_default();
        if current_qdisc != "fq" {
            if let Err(e) = fs::write(PROC_DEFAULT_QDISC, "fq") {
                crate::log_debug!("Failed to set default_qdisc to 'fq': {}", e);
            } else {
                crate::log_info!("Default qdisc set to 'fq' (was '{}')", current_qdisc);
            }
        }
    }

    Ok(Some(target.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_select_congestion_target_auto() {
        let avail_bbr3 = vec![
            "cubic".to_string(),
            "reno".to_string(),
            "bbr3".to_string(),
            "bbr".to_string(),
        ];
        assert_eq!(select_congestion_target("auto", &avail_bbr3), Some("bbr3"));
        assert_eq!(select_congestion_target("", &avail_bbr3), Some("bbr3"));

        let avail_bbr1 = vec!["cubic".to_string(), "reno".to_string(), "bbr".to_string()];
        assert_eq!(select_congestion_target("auto", &avail_bbr1), Some("bbr"));
        assert_eq!(select_congestion_target("", &avail_bbr1), Some("bbr"));

        let avail_none = vec!["cubic".to_string(), "reno".to_string()];
        assert_eq!(select_congestion_target("auto", &avail_none), None);
    }

    #[test]
    fn test_select_congestion_target_explicit() {
        let avail = vec!["cubic".to_string(), "reno".to_string(), "bbr3".to_string()];
        assert_eq!(select_congestion_target("bbr3", &avail), Some("bbr3"));
        assert_eq!(select_congestion_target("BBR3", &avail), Some("bbr3"));
        assert_eq!(select_congestion_target("cubic", &avail), Some("cubic"));
        assert_eq!(select_congestion_target("bbr", &avail), None);
    }

    #[test]
    fn test_select_congestion_target_disabled() {
        let avail = vec!["bbr3".to_string(), "cubic".to_string()];
        assert_eq!(select_congestion_target("off", &avail), None);
        assert_eq!(select_congestion_target("none", &avail), None);
        assert_eq!(select_congestion_target("disabled", &avail), None);
    }
}
