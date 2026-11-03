//! NetlinkWatcher owned struct and VPN tunnel state monitoring.

use std::io;
use std::os::unix::io::{AsRawFd, FromRawFd, OwnedFd, RawFd};

use crate::config::Config;
use crate::context::{AtpdContext, VpnChange, VpnState};
use crate::netlink::bypass::BypassManager;
use crate::netlink::msg::{
    encode_link_dump_request, parse_link_message, parse_xfrm_message, NetlinkMsgIter, XfrmEvent,
    NETLINK_DEBOUNCE_MS, NETLINK_ROUTE, NETLINK_VPN_SETTLE_MS, NETLINK_XFRM, NL_BUF_SIZE,
    NL_DUMP_SIZE, ROUTE_SOCKET_GROUPS, RTM_DELADDR, RTM_DELLINK, RTM_DELROUTE, RTM_DELRULE,
    RTM_NEWADDR, RTM_NEWLINK, RTM_NEWROUTE, RTM_NEWRULE, XFRMNLGRP_SA,
};
use crate::reactor::{Reactor, TimerId};
use crate::{log_info, log_warn};

/// Determines if an interface name is a known VPN or proxy interface prefix.
pub fn is_proxy_interface(ifname: &str) -> bool {
    if ifname.is_empty() {
        return false;
    }
    if ifname.starts_with("ipsec") || ifname.starts_with("xfrm") {
        return true;
    }
    const PREFIXES: &[&str] = &[
        "tun",
        "tap",
        "warp",
        "wg",
        "tailscale",
        "zt",
        "zerotier",
        "utun",
        "vpn",
        "ppp",
    ];
    for &prefix in PREFIXES {
        if ifname.starts_with(prefix) {
            return true;
        }
    }
    false
}

/// Helper that inspects getifaddrs to find an active UP proxy interface.
pub fn getifaddrs_find_vpn() -> Option<String> {
    let mut ifaddrs_ptr: *mut libc::ifaddrs = std::ptr::null_mut();
    // SAFETY: getifaddrs allocates and returns a linked list of network interface structures.
    let ret = unsafe { libc::getifaddrs(&mut ifaddrs_ptr) };
    if ret != 0 || ifaddrs_ptr.is_null() {
        return None;
    }

    let mut fallback: Option<String> = None;
    let mut curr = ifaddrs_ptr;

    while !curr.is_null() {
        // SAFETY: curr is a non-null pointer to a valid ifaddrs node.
        let ifa = unsafe { &*curr };
        if !ifa.ifa_name.is_null() && (ifa.ifa_flags & (libc::IFF_UP as u32)) != 0 {
            // SAFETY: ifa_name is a valid null-terminated C string.
            let c_str = unsafe { std::ffi::CStr::from_ptr(ifa.ifa_name) };
            if let Ok(name) = c_str.to_str() {
                if is_proxy_interface(name) {
                    if name.starts_with("ipsec") {
                        let res = name.to_string();
                        // SAFETY: frees memory allocated by getifaddrs.
                        unsafe { libc::freeifaddrs(ifaddrs_ptr) };
                        return Some(res);
                    }
                    if fallback.is_none() {
                        fallback = Some(name.to_string());
                    }
                }
            }
        }
        curr = ifa.ifa_next;
    }

    // SAFETY: frees memory allocated by getifaddrs.
    unsafe { libc::freeifaddrs(ifaddrs_ptr) };
    fallback
}

/// Dumps links via Netlink to find an active UP proxy interface.
pub fn detect_active_vpn_netlink(sync_fd: Option<RawFd>) -> Option<String> {
    let _owned_tmp: Option<OwnedFd>;
    let fd = match sync_fd {
        Some(f) if f >= 0 => {
            _owned_tmp = None;
            f
        }
        _ => {
            // SAFETY: creates a temporary non-blocking raw netlink socket.
            let raw = unsafe {
                libc::socket(
                    libc::AF_NETLINK,
                    libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                    NETLINK_ROUTE,
                )
            };
            if raw < 0 {
                return None;
            }
            // SAFETY: raw is an open descriptor exclusively owned by this OwnedFd.
            let ofd = unsafe { OwnedFd::from_raw_fd(raw) };
            let fd = ofd.as_raw_fd();
            _owned_tmp = Some(ofd);
            fd
        }
    };

    let req = encode_link_dump_request(1);
    // SAFETY: zeroed sockaddr_nl is valid representation.
    let mut dst_sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    dst_sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;

    crate::netlink::bypass::drain_socket(fd);

    // SAFETY: sending link dump request.
    let sent = unsafe {
        libc::sendto(
            fd,
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

    let mut fallback: Option<String> = None;
    let mut ipsec_found: Option<String> = None;

    let _ = crate::netlink::bypass::recv_all_with_timeout(fd, 1, 50, |msg| {
        if let Some(info) = parse_link_message(msg) {
            if (info.flags & (libc::IFF_UP as u32)) != 0 && is_proxy_interface(&info.name) {
                if info.name.starts_with("ipsec") && ipsec_found.is_none() {
                    ipsec_found = Some(info.name);
                } else if fallback.is_none() {
                    fallback = Some(info.name);
                }
            }
        }
        Ok(())
    });

    ipsec_found.or(fallback)
}

/// Owned NetlinkWatcher responsible for interface and tunnel event monitoring.
pub struct NetlinkWatcher {
    pub route_fd: OwnedFd,
    pub xfrm_fd: Option<OwnedFd>,
    pub sync_fd: Option<OwnedFd>,
    pub route_token: u64,
    pub xfrm_token: u64,
    pub debounce_timer: Option<TimerId>,
    pub bypass: BypassManager,
}

impl NetlinkWatcher {
    /// Opens NETLINK_ROUTE (multicast groups) and NETLINK_XFRM sockets, registering them with the reactor.
    pub fn new(r: &mut Reactor, route_token: u64, xfrm_token: u64) -> io::Result<Self> {
        // 1. Open NETLINK_ROUTE socket
        // SAFETY: creates a non-blocking raw netlink socket with close-on-exec.
        let route_raw = unsafe {
            libc::socket(
                libc::AF_NETLINK,
                libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                NETLINK_ROUTE,
            )
        };
        if route_raw < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: route_raw is a freshly opened descriptor owned exclusively by this OwnedFd.
        let route_fd = unsafe { OwnedFd::from_raw_fd(route_raw) };

        // SAFETY: zeroed sockaddr_nl is valid representation.
        let mut sa_route: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
        sa_route.nl_family = libc::AF_NETLINK as libc::sa_family_t;
        sa_route.nl_groups = ROUTE_SOCKET_GROUPS;

        // SAFETY: binding route socket to multicast groups.
        let bind_ret = unsafe {
            libc::bind(
                route_fd.as_raw_fd(),
                &sa_route as *const _ as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
            )
        };
        if bind_ret < 0 {
            return Err(io::Error::last_os_error());
        }

        let mut buf_size: libc::c_int = NL_DUMP_SIZE as libc::c_int;
        // SAFETY: configuring receive buffer size on route socket.
        let _ = unsafe {
            libc::setsockopt(
                route_fd.as_raw_fd(),
                libc::SOL_SOCKET,
                libc::SO_RCVBUF,
                &mut buf_size as *mut _ as *mut libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            )
        };

        r.add_fd(route_fd.as_raw_fd(), crate::reactor::READ, route_token)?;

        // 2. Open NETLINK_XFRM socket
        // SAFETY: creates a non-blocking raw netlink socket with close-on-exec for XFRM.
        let xfrm_raw = unsafe {
            libc::socket(
                libc::AF_NETLINK,
                libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                NETLINK_XFRM,
            )
        };
        let xfrm_fd = if xfrm_raw >= 0 {
            // SAFETY: xfrm_raw is a freshly opened descriptor owned exclusively by this OwnedFd.
            let fd = unsafe { OwnedFd::from_raw_fd(xfrm_raw) };
            // SAFETY: zeroed sockaddr_nl is valid representation.
            let mut sa_xfrm: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
            sa_xfrm.nl_family = libc::AF_NETLINK as libc::sa_family_t;
            sa_xfrm.nl_groups = XFRMNLGRP_SA;

            // SAFETY: binding xfrm socket to XFRMNLGRP_SA.
            let ret = unsafe {
                libc::bind(
                    fd.as_raw_fd(),
                    &sa_xfrm as *const _ as *const libc::sockaddr,
                    std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
                )
            };
            if ret < 0 {
                log_warn!(
                    "[XFRM] bind() failed: {} (Google VPN auto-detection disabled)",
                    io::Error::last_os_error()
                );
                None
            } else {
                // SAFETY: configuring receive buffer size on XFRM socket.
                let _ = unsafe {
                    libc::setsockopt(
                        fd.as_raw_fd(),
                        libc::SOL_SOCKET,
                        libc::SO_RCVBUF,
                        &mut buf_size as *mut _ as *mut libc::c_void,
                        std::mem::size_of::<libc::c_int>() as libc::socklen_t,
                    )
                };
                if let Err(e) = r.add_fd(fd.as_raw_fd(), crate::reactor::READ, xfrm_token) {
                    log_warn!("[XFRM] failed to register listener with reactor: {}", e);
                    None
                } else {
                    Some(fd)
                }
            }
        } else {
            log_warn!(
                "[XFRM] socket(NETLINK_XFRM) failed: {} (Google VPN auto-detection disabled)",
                io::Error::last_os_error()
            );
            None
        };

        // 3. Open sync socket for route and rule dumps
        // SAFETY: creates a non-blocking raw netlink socket with close-on-exec for sync operations.
        let sync_raw = unsafe {
            libc::socket(
                libc::AF_NETLINK,
                libc::SOCK_RAW | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                NETLINK_ROUTE,
            )
        };
        let sync_fd = if sync_raw >= 0 {
            // SAFETY: sync_raw is a freshly opened descriptor owned exclusively by this OwnedFd.
            let fd = unsafe { OwnedFd::from_raw_fd(sync_raw) };
            // SAFETY: zeroed sockaddr_nl is valid representation.
            let mut sa_sync: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
            sa_sync.nl_family = libc::AF_NETLINK as libc::sa_family_t;
            sa_sync.nl_groups = 0;

            // SAFETY: binding sync socket to group 0.
            let ret = unsafe {
                libc::bind(
                    fd.as_raw_fd(),
                    &sa_sync as *const _ as *const libc::sockaddr,
                    std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
                )
            };
            if ret < 0 {
                None
            } else {
                // SAFETY: configuring receive buffer size on sync socket.
                let _ = unsafe {
                    libc::setsockopt(
                        fd.as_raw_fd(),
                        libc::SOL_SOCKET,
                        libc::SO_RCVBUF,
                        &mut buf_size as *mut _ as *mut libc::c_void,
                        std::mem::size_of::<libc::c_int>() as libc::socklen_t,
                    )
                };
                Some(fd)
            }
        } else {
            None
        };

        log_info!(
            "Netlink initialized (sync_fd={}, async_fd={})",
            sync_fd.as_ref().map_or(-1, |f| f.as_raw_fd()),
            route_fd.as_raw_fd()
        );

        Ok(Self {
            route_fd,
            xfrm_fd,
            sync_fd,
            route_token,
            xfrm_token,
            debounce_timer: None,
            bypass: BypassManager::new(),
        })
    }

    fn schedule_refresh(&mut self, r: &mut Reactor, delay_ms: u64) {
        if let Some(timer_id) = self.debounce_timer.take() {
            r.cancel_timer(timer_id);
        }
        let id = r.add_timer(delay_ms, 0);
        self.debounce_timer = Some(id);
    }

    /// Triggers debounce with NETLINK_DEBOUNCE_MS (500 ms).
    pub fn trigger_debounce(&mut self, r: &mut Reactor) {
        self.schedule_refresh(r, NETLINK_DEBOUNCE_MS);
    }

    /// Triggers VPN settle refresh with NETLINK_VPN_SETTLE_MS (1000 ms), matching C `netlink_refresh_state`.
    pub fn refresh_state(&mut self, r: &mut Reactor) {
        self.schedule_refresh(r, NETLINK_VPN_SETTLE_MS);
    }

    /// Feeds raw bytes received on the route socket through the parse path.
    pub fn handle_route_bytes(&mut self, r: &mut Reactor, buf: &[u8]) {
        for msg in NetlinkMsgIter::new(buf) {
            match msg.nlmsg_type {
                RTM_NEWLINK | RTM_DELLINK | RTM_NEWADDR | RTM_DELADDR | RTM_NEWROUTE
                | RTM_DELROUTE | RTM_NEWRULE | RTM_DELRULE => {
                    self.trigger_debounce(r);
                }
                _ => {}
            }
        }
    }

    /// Feeds raw bytes received on the XFRM socket through the parse path.
    pub fn handle_xfrm_bytes(
        &mut self,
        r: &mut Reactor,
        ctx: &mut AtpdContext,
        buf: &[u8],
    ) -> Vec<VpnChange> {
        let mut changes = Vec::new();
        for msg in NetlinkMsgIter::new(buf) {
            match parse_xfrm_message(&msg) {
                Some(XfrmEvent::NewSa(if_id)) => {
                    let ifname = format!("ipsec{}", if_id.saturating_sub(1));
                    if let Ok(Some(change)) =
                        ctx.vpn_transition(VpnState::Predicting, if_id, Some(&ifname))
                    {
                        changes.push(change);
                    }
                    self.refresh_state(r);
                }
                Some(XfrmEvent::DelSa) => {
                    if let Ok(Some(change)) = ctx.vpn_transition(VpnState::Teardown, 0, None) {
                        changes.push(change);
                    }
                    self.refresh_state(r);
                }
                None => {}
            }
        }
        changes
    }

    /// Handles IO events from Reactor on route_token or xfrm_token.
    pub fn handle_io(
        &mut self,
        r: &mut Reactor,
        ctx: &mut AtpdContext,
        _cfg: &Config,
        token: u64,
    ) -> Vec<VpnChange> {
        if token == self.route_token {
            let mut buf = [0u8; NL_BUF_SIZE];
            loop {
                // SAFETY: reading available bytes from non-blocking route socket.
                let n = unsafe {
                    libc::recv(
                        self.route_fd.as_raw_fd(),
                        buf.as_mut_ptr() as *mut libc::c_void,
                        buf.len(),
                        libc::MSG_DONTWAIT,
                    )
                };
                if n <= 0 {
                    break;
                }
                self.handle_route_bytes(r, &buf[..n as usize]);
            }
            Vec::new()
        } else {
            let xfrm_raw_fd = self.xfrm_fd.as_ref().map(|f| f.as_raw_fd());
            if let Some(fd) = xfrm_raw_fd {
                if token == self.xfrm_token {
                    let mut changes = Vec::new();
                    let mut buf = [0u8; NL_BUF_SIZE];
                    loop {
                        // SAFETY: reading available bytes from non-blocking XFRM socket.
                        let n = unsafe {
                            libc::recv(
                                fd,
                                buf.as_mut_ptr() as *mut libc::c_void,
                                buf.len(),
                                libc::MSG_DONTWAIT,
                            )
                        };
                        if n <= 0 {
                            break;
                        }
                        let mut batch = self.handle_xfrm_bytes(r, ctx, &buf[..n as usize]);
                        changes.append(&mut batch);
                    }
                    return changes;
                }
            }
            Vec::new()
        }
    }

    /// Handles timer expirations: performs active VPN detection and syncs bypass rules.
    pub fn on_timer(
        &mut self,
        _r: &mut Reactor,
        ctx: &mut AtpdContext,
        cfg: &Config,
        id: TimerId,
    ) -> Option<Vec<VpnChange>> {
        if self.debounce_timer != Some(id) {
            return None;
        }
        self.debounce_timer = None;

        let mut changes = Vec::new();
        let vpn = self.get_active_vpn();
        let (new_state, iface_str) = match vpn.as_deref() {
            Some(name) if !name.is_empty() => (VpnState::Ready, Some(name)),
            _ => (VpnState::Idle, None),
        };

        if let Ok(Some(change)) = ctx.vpn_transition(new_state, 0, iface_str) {
            changes.push(change);
        }

        let ready = vpn.as_deref().is_some_and(|name| name.starts_with("ipsec"));
        let sync_fd = self.sync_fd.as_ref().map(|f| f.as_raw_fd());
        self.bypass.apply_config(cfg, sync_fd, vpn.as_deref());
        let _ = self.bypass.sync(ready, sync_fd);

        Some(changes)
    }

    /// Returns the active VPN interface if any is detected.
    pub fn get_active_vpn(&mut self) -> Option<String> {
        if let Some(vpn) = getifaddrs_find_vpn() {
            return Some(vpn);
        }
        detect_active_vpn_netlink(self.sync_fd.as_ref().map(|f| f.as_raw_fd()))
    }

    /// Hot-reloads VPN bypass rules from configuration.
    pub fn apply_vpn_bypass_config(&mut self, cfg: &Config) {
        let active_vpn = self.get_active_vpn();
        self.bypass.apply_config(
            cfg,
            self.sync_fd.as_ref().map(|f| f.as_raw_fd()),
            active_vpn.as_deref(),
        );
    }

    /// Closes and unregisters sockets and timers from the reactor.
    pub fn close(&mut self, r: &mut Reactor) {
        if let Some(id) = self.debounce_timer.take() {
            r.cancel_timer(id);
        }
        let _ = r.remove_fd(self.route_fd.as_raw_fd());
        if let Some(ref xfrm) = self.xfrm_fd {
            let _ = r.remove_fd(xfrm.as_raw_fd());
        }
    }
}
