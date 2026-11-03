//! Network interface statistics via /proc/net/dev with Netlink fallback.

use std::fs;
use std::os::unix::io::{AsRawFd, FromRawFd, OwnedFd};

use crate::netlink::msg::{encode_getlink_stats_request, parse_link_message, NetlinkMsgIter};

/// Pure parser function to parse a single line from `/proc/net/dev`.
/// Format: `  <iface>: <rx_bytes> <rx_packets> ... <tx_bytes> ...`
/// Returns `Some((ifname, rx_bytes, tx_bytes))` on success.
pub fn parse_proc_net_dev_line(line: &str) -> Option<(&str, u64, u64)> {
    let (name_part, stats_part) = line.split_once(':')?;
    let name = name_part.trim();
    if name.is_empty() {
        return None;
    }

    let mut tokens = stats_part.split_ascii_whitespace();
    let rx_bytes = tokens.next()?.parse::<u64>().ok()?;
    for _ in 0..7 {
        tokens.next()?;
    }
    let tx_bytes = tokens.next()?.parse::<u64>().ok()?;

    Some((name, rx_bytes, tx_bytes))
}

/// Retrieves interface statistics (rx_bytes, tx_bytes) for the given interface.
/// Mirrors C `netlink_get_iface_stats`: tries `/proc/net/dev` first, then falls back to Netlink.
pub fn iface_stats(iface: &str) -> Option<(u64, u64)> {
    if iface.is_empty() {
        return None;
    }

    // 1. Fast direct read from /proc/net/dev
    if let Ok(content) = fs::read_to_string("/proc/net/dev") {
        for line in content.lines() {
            if let Some((name, rx, tx)) = parse_proc_net_dev_line(line) {
                if name == iface {
                    return Some((rx, tx));
                }
            }
        }
    }

    // 2. Netlink fallback
    netlink_stats_fallback(iface)
}

fn netlink_stats_fallback(iface: &str) -> Option<(u64, u64)> {
    let c_iface = std::ffi::CString::new(iface).ok()?;
    // SAFETY: c_iface is a valid null-terminated C string.
    let if_index = unsafe { libc::if_nametoindex(c_iface.as_ptr()) };
    if if_index == 0 {
        return None;
    }

    // SAFETY: creates a non-blocking raw netlink socket with close-on-exec.
    let raw_fd = unsafe {
        libc::socket(
            libc::AF_NETLINK,
            libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
            libc::NETLINK_ROUTE,
        )
    };
    if raw_fd < 0 {
        return None;
    }
    // SAFETY: raw_fd is a freshly opened descriptor owned exclusively by this OwnedFd.
    let owned_fd = unsafe { OwnedFd::from_raw_fd(raw_fd) };

    // SAFETY: zeroed sockaddr_nl is valid representation.
    let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;

    // SAFETY: binding socket to group 0 with valid sockaddr_nl structure.
    let bind_ret = unsafe {
        libc::bind(
            owned_fd.as_raw_fd(),
            &sa as *const _ as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
        )
    };
    if bind_ret < 0 {
        return None;
    }

    let req = encode_getlink_stats_request(if_index, 1);
    // SAFETY: zeroed sockaddr_nl is valid representation.
    let mut dst_sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    dst_sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;

    // SAFETY: sending request buffer to kernel netlink endpoint.
    let sent = unsafe {
        libc::sendto(
            owned_fd.as_raw_fd(),
            req.as_ptr() as *const libc::c_void,
            req.len(),
            0,
            &mut dst_sa as *mut _ as *mut libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
        )
    };
    if sent < 0 || (sent as usize) != req.len() {
        return None;
    }

    let mut pfd = libc::pollfd {
        fd: owned_fd.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    };

    // SAFETY: polling 1 descriptor with bounded 50ms wait.
    let pr = unsafe { libc::poll(&mut pfd, 1, 50) };
    if pr <= 0 || (pfd.revents & libc::POLLIN) == 0 {
        return None;
    }

    let mut buf = [0u8; 8192];
    // SAFETY: receiving message into stack buffer with MSG_DONTWAIT.
    let n = unsafe {
        libc::recv(
            owned_fd.as_raw_fd(),
            buf.as_mut_ptr() as *mut libc::c_void,
            buf.len(),
            libc::MSG_DONTWAIT,
        )
    };
    if n <= 0 {
        return None;
    }

    for msg in NetlinkMsgIter::new(&buf[..n as usize]) {
        if let Some(info) = parse_link_message(&msg) {
            if info.name == iface {
                return Some((info.rx_bytes, info.tx_bytes));
            }
        }
    }

    None
}
