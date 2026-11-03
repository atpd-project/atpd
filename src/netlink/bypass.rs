//! VPN bypass routing rules synchronization and hot-reload.

use std::io;
use std::os::unix::io::RawFd;

use crate::config::{Config, VpnBypassRoute, ATP_MAX_VPN_BYPASS_ROUTES};
use crate::netlink::msg::{
    encode_route_dump_request, encode_rule_dump_request, encode_rule_request, parse_route_message,
    parse_rule_message, NetlinkMsg, NetlinkMsgIter, RouteFindCtx, RuleDumpCtx, AF_INET, AF_INET6,
    NLMSG_DONE, NLMSG_ERROR, NL_DUMP_SIZE, RTM_DELRULE, RTM_NEWRULE,
};
use crate::{log_error, log_info, log_warn};

/// Pure helper to resolve interface index to interface name.
pub fn if_indextoname_safe(idx: u32) -> Option<String> {
    let mut buf = [0u8; libc::IF_NAMESIZE];
    // SAFETY: buf is a valid writable buffer of length IF_NAMESIZE.
    let ptr = unsafe { libc::if_indextoname(idx, buf.as_mut_ptr() as *mut libc::c_char) };
    if ptr.is_null() {
        None
    } else {
        let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
        Some(String::from_utf8_lossy(&buf[..len]).into_owned())
    }
}

/// Drains unread pending messages from a non-blocking Netlink socket.
pub fn drain_socket(sock_fd: RawFd) {
    if sock_fd < 0 {
        return;
    }
    let mut buf = [0u8; 1024];
    loop {
        // SAFETY: draining pending unread bytes from non-blocking socket.
        let n = unsafe {
            libc::recv(
                sock_fd,
                buf.as_mut_ptr() as *mut libc::c_void,
                buf.len(),
                libc::MSG_DONTWAIT,
            )
        };
        if n <= 0 {
            break;
        }
    }
}

/// Receives Netlink responses with a bounded timeout, invoking a parser for matching messages.
pub fn recv_all_with_timeout<F>(
    sock_fd: RawFd,
    expected_seq: u32,
    timeout_ms: i32,
    mut parser: F,
) -> io::Result<()>
where
    F: FnMut(&NetlinkMsg<'_>) -> io::Result<()>,
{
    if sock_fd < 0 {
        return Err(io::Error::from_raw_os_error(libc::EBADF));
    }

    let mut s_dump_buf = [0u8; NL_DUMP_SIZE];
    let mut done = false;

    while !done {
        let mut pfd = libc::pollfd {
            fd: sock_fd,
            events: libc::POLLIN,
            revents: 0,
        };
        let wait_ms = if timeout_ms > 0 { timeout_ms } else { 50 };
        // SAFETY: polling 1 descriptor with wait_ms.
        let pr = unsafe { libc::poll(&mut pfd, 1, wait_ms) };
        if pr < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            log_error!("Netlink: poll failed: {}", err);
            return Err(err);
        }
        if pr == 0 {
            log_warn!("Netlink: poll timeout ({} ms)", wait_ms);
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Netlink poll timeout",
            ));
        }
        if (pfd.revents & libc::POLLIN) == 0 {
            log_warn!("Netlink: poll unexpected revents 0x{:x}", pfd.revents);
            return Err(io::Error::other("unexpected revents"));
        }

        // SAFETY: receiving into buffer with MSG_DONTWAIT.
        let len = unsafe {
            libc::recv(
                sock_fd,
                s_dump_buf.as_mut_ptr() as *mut libc::c_void,
                s_dump_buf.len(),
                libc::MSG_DONTWAIT,
            )
        };
        if len < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::EINTR)
                || err.raw_os_error() == Some(libc::EAGAIN)
                || err.raw_os_error() == Some(libc::EWOULDBLOCK)
            {
                continue;
            }
            log_error!("Netlink: recv failed: {}", err);
            return Err(err);
        }
        if len == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "netlink socket closed",
            ));
        }

        for msg in NetlinkMsgIter::new(&s_dump_buf[..len as usize]) {
            if msg.nlmsg_seq != expected_seq {
                continue;
            }
            if msg.nlmsg_type == NLMSG_DONE {
                done = true;
                break;
            }
            if msg.nlmsg_type == NLMSG_ERROR {
                if msg.payload.len() >= 4 {
                    let err_code = i32::from_ne_bytes([
                        msg.payload[0],
                        msg.payload[1],
                        msg.payload[2],
                        msg.payload[3],
                    ]);
                    if err_code != 0 {
                        if err_code == -libc::EEXIST {
                            done = true;
                            break;
                        }
                        log_error!(
                            "Netlink: error response {}: {}",
                            err_code,
                            io::Error::from_raw_os_error(-err_code)
                        );
                        return Err(io::Error::from_raw_os_error(-err_code));
                    }
                }
                done = true;
                break;
            }
            parser(&msg)?;
        }
    }

    Ok(())
}

/// Sends an RTM_NEWRULE or RTM_DELRULE Netlink request.
#[allow(clippy::too_many_arguments)]
pub fn netlink_rule_send(
    sock_fd: RawFd,
    cmd: u16,
    family: i32,
    dst_len: u8,
    addr: &[u8],
    table: u32,
    pref: u32,
    seq: &mut u32,
) -> io::Result<()> {
    if sock_fd < 0 {
        return Err(io::Error::from_raw_os_error(libc::EBADF));
    }

    *seq = seq.wrapping_add(1);
    let cur_seq = *seq;
    let expected_len = if family == AF_INET { 4 } else { 16 };
    let req = encode_rule_request(
        cmd,
        family,
        dst_len,
        &addr[..expected_len.min(addr.len())],
        table,
        pref,
        cur_seq,
    );

    drain_socket(sock_fd);

    // SAFETY: zeroed sockaddr_nl is valid representation.
    let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
    sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;

    // SAFETY: sending rule request to kernel netlink endpoint.
    let sent = unsafe {
        libc::sendto(
            sock_fd,
            req.as_ptr() as *const libc::c_void,
            req.len(),
            0,
            &mut sa as *mut _ as *mut libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
        )
    };
    if sent < 0 || (sent as usize) != req.len() {
        let err = io::Error::last_os_error();
        log_warn!(
            "Netlink: failed to send rule request (cmd {}): {}",
            cmd,
            err
        );
        return Err(err);
    }

    recv_all_with_timeout(sock_fd, cur_seq, 50, |_| Ok(()))
}

/// Ported `vpn_bypass_sync`: synchronizes kernel IP rules against desired bypass routes.
pub fn vpn_bypass_sync(
    sock_fd: Option<RawFd>,
    mut ready: bool,
    routes: &[VpnBypassRoute],
    pref: u32,
    seq: &mut u32,
    no_candidate_warned: &mut bool,
) -> io::Result<()> {
    if routes.is_empty() {
        return Ok(());
    }
    let fd = match sock_fd {
        Some(f) if f >= 0 => f,
        _ => return Ok(()),
    };

    let mut r_ctx = RouteFindCtx::default();
    let mut selected_table = 0u32;
    let mut selected_ifname = String::new();

    if ready {
        *seq = seq.wrapping_add(1);
        let cur_seq = *seq;
        let req = encode_route_dump_request(AF_INET, cur_seq);

        drain_socket(fd);
        // SAFETY: zeroed sockaddr_nl is valid representation.
        let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
        sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;

        // SAFETY: sending route dump request to kernel netlink endpoint.
        let sent = unsafe {
            libc::sendto(
                fd,
                req.as_ptr() as *const libc::c_void,
                req.len(),
                0,
                &mut sa as *mut _ as *mut libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
            )
        };
        if sent < 0
            || (sent as usize) != req.len()
            || recv_all_with_timeout(fd, cur_seq, 50, |msg| {
                parse_route_message(msg, if_indextoname_safe, &mut r_ctx);
                Ok(())
            })
            .is_err()
        {
            log_warn!("VPN bypass: route dump failed, leaving rules unchanged");
            return Ok(());
        }

        if !r_ctx.found_candidate {
            // Try IPv6 default route if IPv4 didn't provide one
            drain_socket(fd);
            *seq = seq.wrapping_add(1);
            let cur_v6_seq = *seq;
            let req_v6 = encode_route_dump_request(AF_INET6, cur_v6_seq);

            // SAFETY: sending IPv6 route dump request to kernel netlink endpoint.
            let sent_v6 = unsafe {
                libc::sendto(
                    fd,
                    req_v6.as_ptr() as *const libc::c_void,
                    req_v6.len(),
                    0,
                    &mut sa as *mut _ as *mut libc::sockaddr,
                    std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
                )
            };
            if sent_v6 < 0
                || (sent_v6 as usize) != req_v6.len()
                || recv_all_with_timeout(fd, cur_v6_seq, 50, |msg| {
                    parse_route_message(msg, if_indextoname_safe, &mut r_ctx);
                    Ok(())
                })
                .is_err()
            {
                log_warn!("VPN bypass: IPv6 route dump failed, leaving rules unchanged");
                return Ok(());
            }
        }

        if !r_ctx.found_candidate {
            if !*no_candidate_warned {
                log_warn!("VPN bypass: physical default route table not found");
                *no_candidate_warned = true;
            }
            ready = false;
        } else {
            *no_candidate_warned = false;
            selected_table = r_ctx.selected_table;
            selected_ifname = r_ctx.selected_ifname.clone();
        }
    } else {
        *no_candidate_warned = false;
    }

    let mut rule_ctx = RuleDumpCtx::new();
    let families = [AF_INET, AF_INET6];
    for &fam in &families {
        *seq = seq.wrapping_add(1);
        let cur_seq = *seq;
        let req = encode_rule_dump_request(fam, cur_seq);

        drain_socket(fd);
        // SAFETY: zeroed sockaddr_nl is valid representation.
        let mut sa: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
        sa.nl_family = libc::AF_NETLINK as libc::sa_family_t;

        // SAFETY: sending rule dump request to kernel netlink endpoint.
        let sent = unsafe {
            libc::sendto(
                fd,
                req.as_ptr() as *const libc::c_void,
                req.len(),
                0,
                &mut sa as *mut _ as *mut libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t,
            )
        };
        if sent < 0
            || (sent as usize) != req.len()
            || recv_all_with_timeout(fd, cur_seq, 50, |msg| {
                parse_rule_message(msg, routes, pref, &mut rule_ctx);
                Ok(())
            })
            .is_err()
        {
            log_warn!("VPN bypass: rule dump failed, leaving rules unchanged");
            return Ok(());
        }
    }

    let mut rules_added = 0;
    let mut rules_removed = 0;

    if ready {
        for (i, route) in routes.iter().enumerate() {
            let mut has_table_t = false;
            for dumped in &rule_ctx.rules {
                if dumped.route_idx == i {
                    if dumped.table == selected_table {
                        if !has_table_t {
                            has_table_t = true;
                        } else if netlink_rule_send(
                            fd,
                            RTM_DELRULE,
                            dumped.family,
                            dumped.dst_len,
                            &dumped.addr,
                            dumped.table,
                            pref,
                            seq,
                        )
                        .is_ok()
                        {
                            rules_removed += 1;
                        }
                    } else if netlink_rule_send(
                        fd,
                        RTM_DELRULE,
                        dumped.family,
                        dumped.dst_len,
                        &dumped.addr,
                        dumped.table,
                        pref,
                        seq,
                    )
                    .is_ok()
                    {
                        rules_removed += 1;
                    }
                }
            }
            if !has_table_t
                && netlink_rule_send(
                    fd,
                    RTM_NEWRULE,
                    route.family,
                    route.prefix_len,
                    &route.addr,
                    selected_table,
                    pref,
                    seq,
                )
                .is_ok()
            {
                rules_added += 1;
            }
        }
    } else {
        for dumped in &rule_ctx.rules {
            if netlink_rule_send(
                fd,
                RTM_DELRULE,
                dumped.family,
                dumped.dst_len,
                &dumped.addr,
                dumped.table,
                pref,
                seq,
            )
            .is_ok()
            {
                rules_removed += 1;
            }
        }
    }

    if rules_added > 0 {
        log_info!(
            "VPN bypass: added {} rule(s) to table {} ({})",
            rules_added,
            selected_table,
            selected_ifname
        );
    }
    if rules_removed > 0 {
        log_info!("VPN bypass: removed {} rule(s)", rules_removed);
    }

    Ok(())
}

/// Tracks bypass routing rules state and synchronization.
pub struct BypassManager {
    pub routes: Vec<VpnBypassRoute>,
    pub pref: u32,
    pub seq: u32,
    pub no_candidate_warned: bool,
}

impl Default for BypassManager {
    fn default() -> Self {
        Self::new()
    }
}

impl BypassManager {
    pub fn new() -> Self {
        Self {
            routes: Vec::new(),
            pref: 12400,
            seq: 0,
            no_candidate_warned: false,
        }
    }

    pub fn sync(&mut self, ready: bool, sock_fd: Option<RawFd>) -> io::Result<()> {
        vpn_bypass_sync(
            sock_fd,
            ready,
            &self.routes,
            self.pref,
            &mut self.seq,
            &mut self.no_candidate_warned,
        )
    }

    pub fn apply_config(
        &mut self,
        cfg: &Config,
        sock_fd: Option<RawFd>,
        active_vpn: Option<&str>,
    ) -> io::Result<()> {
        let new_count = cfg
            .interface
            .vpn_bypass_route_count
            .min(ATP_MAX_VPN_BYPASS_ROUTES);
        let new_pref = if cfg.interface.vpn_bypass_pref != 0 {
            cfg.interface.vpn_bypass_pref as u32
        } else {
            12400
        };
        let new_routes: Vec<VpnBypassRoute> = cfg.interface.vpn_bypass_routes[..new_count].to_vec();

        let routes_changed = self.routes != new_routes;
        let pref_changed = self.pref != new_pref;

        if !routes_changed && !pref_changed {
            return Ok(());
        }

        let ready = active_vpn.is_some_and(|vpn| vpn.starts_with("ipsec"));

        if pref_changed {
            let _ = vpn_bypass_sync(
                sock_fd,
                false,
                &self.routes,
                self.pref,
                &mut self.seq,
                &mut self.no_candidate_warned,
            );
            self.pref = new_pref;
            self.routes = new_routes;
            let _ = vpn_bypass_sync(
                sock_fd,
                ready,
                &self.routes,
                self.pref,
                &mut self.seq,
                &mut self.no_candidate_warned,
            );
            return Ok(());
        }

        // Preference unchanged: delete removed routes individually without flapping surviving routes
        let old_routes = self.routes.clone();
        for old_route in &old_routes {
            let found = new_routes.iter().any(|nr| {
                let len = if nr.family == AF_INET { 4 } else { 16 };
                nr.family == old_route.family
                    && nr.prefix_len == old_route.prefix_len
                    && nr.addr[..len] == old_route.addr[..len]
            });
            if !found {
                let single = [*old_route];
                let _ = vpn_bypass_sync(
                    sock_fd,
                    false,
                    &single,
                    self.pref,
                    &mut self.seq,
                    &mut self.no_candidate_warned,
                );
            }
        }

        self.routes = new_routes;
        let _ = vpn_bypass_sync(
            sock_fd,
            ready,
            &self.routes,
            self.pref,
            &mut self.seq,
            &mut self.no_candidate_warned,
        );

        Ok(())
    }
}
