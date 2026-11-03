//! Netlink packet definitions, alignment helpers, pure encoders, and decoders.

use crate::config::VpnBypassRoute;

// Netlink families
pub const NETLINK_ROUTE: i32 = libc::NETLINK_ROUTE;
pub const NETLINK_XFRM: i32 = 6;

// Netlink multicast groups
pub const RTMGRP_LINK: u32 = 1;
pub const RTMGRP_IPV4_IFADDR: u32 = 0x10;
pub const RTMGRP_IPV6_IFADDR: u32 = 0x100;
pub const RTMGRP_IPV4_ROUTE: u32 = 0x40;
pub const RTMGRP_IPV6_ROUTE: u32 = 0x400;
pub const RTMGRP_IPV4_RULE: u32 = 0x80;
pub const RTNLGRP_IPV6_RULE: u32 = 19;

/// Multicast groups listened to by ATPD's NETLINK_ROUTE watcher.
pub const ROUTE_SOCKET_GROUPS: u32 = RTMGRP_LINK
    | RTMGRP_IPV4_IFADDR
    | RTMGRP_IPV6_IFADDR
    | RTMGRP_IPV4_ROUTE
    | RTMGRP_IPV6_ROUTE
    | RTMGRP_IPV4_RULE
    | (1u32 << (RTNLGRP_IPV6_RULE - 1));

pub const XFRMNLGRP_SA: u32 = 3;

// Message types
pub const RTM_NEWLINK: u16 = 16;
pub const RTM_DELLINK: u16 = 17;
pub const RTM_GETLINK: u16 = 18;
pub const RTM_NEWADDR: u16 = 20;
pub const RTM_DELADDR: u16 = 21;
pub const RTM_NEWROUTE: u16 = 24;
pub const RTM_DELROUTE: u16 = 25;
pub const RTM_GETROUTE: u16 = 26;
pub const RTM_NEWRULE: u16 = 32;
pub const RTM_DELRULE: u16 = 33;
pub const RTM_GETRULE: u16 = 34;

pub const NLMSG_DONE: u16 = 3;
pub const NLMSG_ERROR: u16 = 2;

pub const XFRM_MSG_NEWSA: u16 = 16;
pub const XFRM_MSG_DELSA: u16 = 17;
pub const XFRMA_IF_ID: u16 = 28;

// Flags
pub const NLM_F_REQUEST: u16 = 0x01;
pub const NLM_F_DUMP: u16 = 0x300;
pub const NLM_F_ACK: u16 = 0x04;
pub const NLM_F_CREATE: u16 = 0x400;
pub const NLM_F_EXCL: u16 = 0x200;

// FIB Rule constants
pub const FR_ACT_TO_TBL: u8 = 1;
pub const RT_TABLE_UNSPEC: u8 = 0;

pub const FRA_DST: u16 = 1;
pub const FRA_PRIORITY: u16 = 6;
pub const FRA_SUPPRESS_IFGROUP: u16 = 13;
pub const FRA_SUPPRESS_PREFIXLEN: u16 = 14;
pub const FRA_TABLE: u16 = 15;
pub const FRA_PAD: u16 = 18;
pub const FRA_PROTOCOL: u16 = 21;

// Route attribute constants
pub const RTA_TABLE: u16 = 15;
pub const RTA_OIF: u16 = 4;

// Link attribute constants
pub const IFLA_IFNAME: u16 = 3;

pub const AF_INET: i32 = libc::AF_INET;
pub const AF_INET6: i32 = libc::AF_INET6;

pub const NL_BUF_SIZE: usize = 8192;
pub const NL_DUMP_SIZE: usize = 32768;
pub const NETLINK_DEBOUNCE_MS: u64 = 500;
pub const NETLINK_VPN_SETTLE_MS: u64 = 1000;

#[inline]
pub fn nlmsg_align(len: usize) -> usize {
    (len + 3) & !3
}

#[inline]
pub fn rta_align(len: usize) -> usize {
    (len + 3) & !3
}

#[inline]
pub fn nlmsg_length(len: usize) -> usize {
    16 + len
}

#[inline]
pub fn rta_length(len: usize) -> usize {
    4 + len
}

/// Appends a Netlink attribute (`struct rtattr`) with 4-byte alignment,
/// updating the message header length (`nlmsg_len`) in `buf[0..4]`.
pub fn netlink_add_attr(buf: &mut Vec<u8>, attr_type: u16, data: &[u8]) {
    let start_offset = buf.len();
    let rtalen = rta_length(data.len());
    let new_len = nlmsg_align(start_offset) + rta_align(rtalen);
    buf.resize(new_len, 0);

    let attr_offset = nlmsg_align(start_offset);
    let rta_len_bytes = (rtalen as u16).to_ne_bytes();
    let rta_type_bytes = attr_type.to_ne_bytes();

    buf[attr_offset..attr_offset + 2].copy_from_slice(&rta_len_bytes);
    buf[attr_offset + 2..attr_offset + 4].copy_from_slice(&rta_type_bytes);
    if !data.is_empty() {
        buf[attr_offset + 4..attr_offset + 4 + data.len()].copy_from_slice(data);
    }

    let total_len = new_len as u32;
    buf[0..4].copy_from_slice(&total_len.to_ne_bytes());
}

/// Pure function to encode an RTM_NEWRULE or RTM_DELRULE request.
pub fn encode_rule_request(
    cmd: u16,
    family: i32,
    dst_len: u8,
    addr: &[u8],
    table: u32,
    pref: u32,
    seq: u32,
) -> Vec<u8> {
    let mut buf = Vec::with_capacity(128);
    let initial_len = nlmsg_length(12);
    buf.resize(initial_len, 0);

    let flags = if cmd == RTM_NEWRULE {
        NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL
    } else {
        NLM_F_REQUEST | NLM_F_ACK
    };

    // struct nlmsghdr (16 bytes)
    buf[0..4].copy_from_slice(&(initial_len as u32).to_ne_bytes());
    buf[4..6].copy_from_slice(&cmd.to_ne_bytes());
    buf[6..8].copy_from_slice(&flags.to_ne_bytes());
    buf[8..12].copy_from_slice(&seq.to_ne_bytes());
    buf[12..16].copy_from_slice(&0u32.to_ne_bytes());

    // struct fib_rule_hdr (12 bytes at offset 16)
    buf[16] = family as u8;
    buf[17] = dst_len;
    buf[18] = 0; // src_len
    buf[19] = 0; // tos
    buf[20] = if table < 256 {
        table as u8
    } else {
        RT_TABLE_UNSPEC
    };
    buf[21] = 0; // res1
    buf[22] = 0; // res2
    buf[23] = FR_ACT_TO_TBL;
    buf[24..28].copy_from_slice(&0u32.to_ne_bytes()); // flags

    // Attributes
    netlink_add_attr(&mut buf, FRA_DST, addr);
    netlink_add_attr(&mut buf, FRA_PRIORITY, &pref.to_ne_bytes());
    netlink_add_attr(&mut buf, FRA_TABLE, &table.to_ne_bytes());

    buf
}

/// Pure function to encode an RTM_GETROUTE dump request.
pub fn encode_route_dump_request(family: i32, seq: u32) -> Vec<u8> {
    let mut buf = vec![0u8; nlmsg_length(12)];
    let len = buf.len() as u32;

    buf[0..4].copy_from_slice(&len.to_ne_bytes());
    buf[4..6].copy_from_slice(&RTM_GETROUTE.to_ne_bytes());
    buf[6..8].copy_from_slice(&(NLM_F_REQUEST | NLM_F_DUMP).to_ne_bytes());
    buf[8..12].copy_from_slice(&seq.to_ne_bytes());

    // struct rtmsg at offset 16
    buf[16] = family as u8;
    buf[20] = RT_TABLE_UNSPEC;

    buf
}

/// Pure function to encode an RTM_GETRULE dump request.
pub fn encode_rule_dump_request(family: i32, seq: u32) -> Vec<u8> {
    let mut buf = vec![0u8; nlmsg_length(12)];
    let len = buf.len() as u32;

    buf[0..4].copy_from_slice(&len.to_ne_bytes());
    buf[4..6].copy_from_slice(&RTM_GETRULE.to_ne_bytes());
    buf[6..8].copy_from_slice(&(NLM_F_REQUEST | NLM_F_DUMP).to_ne_bytes());
    buf[8..12].copy_from_slice(&seq.to_ne_bytes());

    // struct fib_rule_hdr at offset 16
    buf[16] = family as u8;

    buf
}

/// Pure function to encode an RTM_GETLINK dump request.
pub fn encode_link_dump_request(seq: u32) -> Vec<u8> {
    let mut buf = vec![0u8; nlmsg_length(16)];
    let len = buf.len() as u32;

    buf[0..4].copy_from_slice(&len.to_ne_bytes());
    buf[4..6].copy_from_slice(&RTM_GETLINK.to_ne_bytes());
    buf[6..8].copy_from_slice(&(NLM_F_REQUEST | NLM_F_DUMP).to_ne_bytes());
    buf[8..12].copy_from_slice(&seq.to_ne_bytes());

    // struct ifinfomsg at offset 16 (ifi_family = AF_PACKET = 0)

    buf
}

#[derive(Debug, Clone, Copy)]
pub struct NetlinkMsg<'a> {
    pub nlmsg_type: u16,
    pub nlmsg_seq: u32,
    pub payload: &'a [u8],
}

pub struct NetlinkMsgIter<'a> {
    buf: &'a [u8],
    offset: usize,
}

impl<'a> NetlinkMsgIter<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Self { buf, offset: 0 }
    }
}

impl<'a> Iterator for NetlinkMsgIter<'a> {
    type Item = NetlinkMsg<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.offset + 16 > self.buf.len() {
            return None;
        }
        let chunk = &self.buf[self.offset..];
        let nlmsg_len = u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
        let nlmsg_type = u16::from_ne_bytes([chunk[4], chunk[5]]);
        let nlmsg_seq = u32::from_ne_bytes([chunk[8], chunk[9], chunk[10], chunk[11]]);

        let len_usize = nlmsg_len as usize;
        if len_usize < 16 || self.offset + len_usize > self.buf.len() {
            return None;
        }

        let payload = &self.buf[self.offset + 16..self.offset + len_usize];
        let next_offset = self.offset + nlmsg_align(len_usize);
        self.offset = next_offset;

        Some(NetlinkMsg {
            nlmsg_type,
            nlmsg_seq,
            payload,
        })
    }
}

#[derive(Debug, Clone, Copy)]
pub struct NetlinkAttr<'a> {
    pub rta_type: u16,
    pub payload: &'a [u8],
}

pub struct NetlinkAttrIter<'a> {
    buf: &'a [u8],
    offset: usize,
}

impl<'a> NetlinkAttrIter<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Self { buf, offset: 0 }
    }
}

impl<'a> Iterator for NetlinkAttrIter<'a> {
    type Item = NetlinkAttr<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.offset + 4 > self.buf.len() {
            return None;
        }
        let chunk = &self.buf[self.offset..];
        let rta_len = u16::from_ne_bytes([chunk[0], chunk[1]]) as usize;
        let rta_type = u16::from_ne_bytes([chunk[2], chunk[3]]);

        if rta_len < 4 || self.offset + rta_len > self.buf.len() {
            return None;
        }

        let payload = &self.buf[self.offset + 4..self.offset + rta_len];
        let next_offset = self.offset + rta_align(rta_len);
        self.offset = next_offset;

        Some(NetlinkAttr { rta_type, payload })
    }
}

pub const MAX_DUMPED_OURS_RULES: usize = 64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DumpedOursRule {
    pub family: i32,
    pub dst_len: u8,
    pub addr: [u8; 16],
    pub table: u32,
    pub route_idx: usize,
}

#[derive(Debug, Default, Clone)]
pub struct RuleDumpCtx {
    pub rules: Vec<DumpedOursRule>,
}

impl RuleDumpCtx {
    pub fn new() -> Self {
        Self {
            rules: Vec::with_capacity(MAX_DUMPED_OURS_RULES),
        }
    }
}

/// Pure parser function mirroring C `parser_rule_dump`.
/// Matches RTM_NEWRULE messages dumped from the kernel against our configured bypass routes and pref.
pub fn parse_rule_message(
    msg: &NetlinkMsg<'_>,
    bypass_routes: &[VpnBypassRoute],
    bypass_pref: u32,
    ctx: &mut RuleDumpCtx,
) -> bool {
    if msg.nlmsg_type != RTM_NEWRULE {
        return false;
    }
    if msg.payload.len() < 12 {
        return false;
    }

    let family = msg.payload[0] as i32;
    let dst_len = msg.payload[1];
    let src_len = msg.payload[2];
    let tos = msg.payload[3];
    let mut table = msg.payload[4] as u32;
    let action = msg.payload[7];

    if family != AF_INET && family != AF_INET6 {
        return false;
    }
    if action != FR_ACT_TO_TBL || src_len != 0 || tos != 0 {
        return false;
    }

    let attrs_buf = if msg.payload.len() > 12 {
        let align_hdr = nlmsg_align(12);
        if align_hdr <= msg.payload.len() {
            &msg.payload[align_hdr..]
        } else {
            &[]
        }
    } else {
        &[]
    };

    let mut priority = 0u32;
    let mut has_priority = false;
    let mut dst_addr: Option<&[u8]> = None;
    let mut has_other_selectors = false;

    for attr in NetlinkAttrIter::new(attrs_buf) {
        match attr.rta_type {
            FRA_PRIORITY => {
                if attr.payload.len() >= 4 {
                    priority = u32::from_ne_bytes([
                        attr.payload[0],
                        attr.payload[1],
                        attr.payload[2],
                        attr.payload[3],
                    ]);
                    has_priority = true;
                }
            }
            FRA_TABLE => {
                if attr.payload.len() >= 4 {
                    table = u32::from_ne_bytes([
                        attr.payload[0],
                        attr.payload[1],
                        attr.payload[2],
                        attr.payload[3],
                    ]);
                }
            }
            FRA_DST => {
                dst_addr = Some(attr.payload);
            }
            FRA_SUPPRESS_PREFIXLEN | FRA_SUPPRESS_IFGROUP => {
                if attr.payload.len() < 4 {
                    has_other_selectors = true;
                } else {
                    let val = u32::from_ne_bytes([
                        attr.payload[0],
                        attr.payload[1],
                        attr.payload[2],
                        attr.payload[3],
                    ]);
                    if val != u32::MAX {
                        has_other_selectors = true;
                    }
                }
            }
            FRA_PROTOCOL | FRA_PAD => {}
            _ => {
                has_other_selectors = true;
            }
        }
    }

    if has_other_selectors || !has_priority || priority != bypass_pref {
        return false;
    }

    let dst_bytes = match dst_addr {
        Some(b) => b,
        None => return false,
    };
    let expected_len = if family == AF_INET { 4 } else { 16 };
    if dst_bytes.len() < expected_len {
        return false;
    }

    let mut matched_idx: Option<usize> = None;
    for (i, r) in bypass_routes.iter().enumerate() {
        if r.family == family
            && r.prefix_len == dst_len
            && r.addr[..expected_len] == dst_bytes[..expected_len]
        {
            matched_idx = Some(i);
            break;
        }
    }

    let route_idx = match matched_idx {
        Some(idx) => idx,
        None => return false,
    };

    if ctx.rules.len() < MAX_DUMPED_OURS_RULES {
        let mut addr_buf = [0u8; 16];
        addr_buf[..expected_len].copy_from_slice(&dst_bytes[..expected_len]);
        ctx.rules.push(DumpedOursRule {
            family,
            dst_len,
            addr: addr_buf,
            table,
            route_idx,
        });
        true
    } else {
        false
    }
}

#[derive(Debug, Default, Clone)]
pub struct RouteFindCtx {
    pub selected_table: u32,
    pub selected_ifname: String,
    pub found_candidate: bool,
    pub found_wlan: bool,
}

/// Pure parser function mirroring C `parser_route_find`.
pub fn parse_route_message<F>(
    msg: &NetlinkMsg<'_>,
    ifname_lookup: F,
    ctx: &mut RouteFindCtx,
) -> bool
where
    F: Fn(u32) -> Option<String>,
{
    if msg.nlmsg_type != RTM_NEWROUTE || msg.payload.len() < 12 {
        return false;
    }

    let family = msg.payload[0] as i32;
    let dst_len = msg.payload[1];
    if (family != AF_INET && family != AF_INET6) || dst_len != 0 {
        return false;
    }

    let mut table = msg.payload[4] as u32;
    let mut oif = 0i32;
    let mut has_oif = false;

    let attrs_buf = if msg.payload.len() > 12 {
        let align_hdr = nlmsg_align(12);
        if align_hdr <= msg.payload.len() {
            &msg.payload[align_hdr..]
        } else {
            &[]
        }
    } else {
        &[]
    };

    for attr in NetlinkAttrIter::new(attrs_buf) {
        if attr.rta_type == RTA_TABLE && attr.payload.len() >= 4 {
            table = u32::from_ne_bytes([
                attr.payload[0],
                attr.payload[1],
                attr.payload[2],
                attr.payload[3],
            ]);
        } else if attr.rta_type == RTA_OIF && attr.payload.len() >= 4 {
            oif = i32::from_ne_bytes([
                attr.payload[0],
                attr.payload[1],
                attr.payload[2],
                attr.payload[3],
            ]);
            has_oif = true;
        }
    }

    if !has_oif || table < 256 || oif <= 0 {
        return false;
    }

    let ifname = match ifname_lookup(oif as u32) {
        Some(name) => name,
        None => return false,
    };

    if ifname == "lo" || crate::netlink::is_proxy_interface(&ifname) {
        return false;
    }

    if !ctx.found_candidate {
        ctx.selected_table = table;
        ctx.selected_ifname = ifname.clone();
        ctx.found_candidate = true;
        if ifname.starts_with("wlan") {
            ctx.found_wlan = true;
        }
    } else if !ctx.found_wlan && ifname.starts_with("wlan") {
        ctx.selected_table = table;
        ctx.selected_ifname = ifname;
        ctx.found_wlan = true;
    }

    true
}

#[derive(Debug, Default, Clone)]
pub struct LinkInfo {
    pub name: String,
    pub flags: u32,
}

/// Pure parser function mirroring C `parser_link_sync`.
pub fn parse_link_message(msg: &NetlinkMsg<'_>) -> Option<LinkInfo> {
    if msg.nlmsg_type != RTM_NEWLINK || msg.payload.len() < 16 {
        return None;
    }

    let flags = u32::from_ne_bytes([
        msg.payload[8],
        msg.payload[9],
        msg.payload[10],
        msg.payload[11],
    ]);

    let mut name = String::new();

    let attrs_buf = if msg.payload.len() > 16 {
        let align_hdr = nlmsg_align(16);
        if align_hdr <= msg.payload.len() {
            &msg.payload[align_hdr..]
        } else {
            &[]
        }
    } else {
        &[]
    };

    for attr in NetlinkAttrIter::new(attrs_buf) {
        if attr.rta_type == IFLA_IFNAME {
            let len = attr
                .payload
                .iter()
                .position(|&b| b == 0)
                .unwrap_or(attr.payload.len());
            name = String::from_utf8_lossy(&attr.payload[..len]).into_owned();
        }
    }

    if name.is_empty() {
        None
    } else {
        Some(LinkInfo { name, flags })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum XfrmEvent {
    NewSa(u32),
    DelSa,
}

/// Pure parser function mirroring XFRM event parsing from `netlink_xfrm_event_cb`.
pub fn parse_xfrm_message(msg: &NetlinkMsg<'_>) -> Option<XfrmEvent> {
    if msg.nlmsg_type == XFRM_MSG_NEWSA {
        // struct xfrm_usersa_info is 224 bytes
        if msg.payload.len() < 224 {
            return None;
        }
        let attrs_buf = &msg.payload[224..];
        for attr in NetlinkAttrIter::new(attrs_buf) {
            if attr.rta_type == XFRMA_IF_ID && attr.payload.len() >= 4 {
                let if_id = u32::from_ne_bytes([
                    attr.payload[0],
                    attr.payload[1],
                    attr.payload[2],
                    attr.payload[3],
                ]);
                if if_id != 0 {
                    return Some(XfrmEvent::NewSa(if_id));
                }
            }
        }
        None
    } else if msg.nlmsg_type == XFRM_MSG_DELSA {
        Some(XfrmEvent::DelSa)
    } else {
        None
    }
}

#[cfg(test)]
mod fuzz_tests {
    use super::*;

    // With panic = "abort", a parser panic on a malformed kernel message would kill the daemon.
    #[test]
    fn parsers_never_panic_on_random_or_mutated_input() {
        let mut seed: u64 = 0x9e37_79b9_7f4a_7c15;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let types = [16u16, 17, 24, 25, 32, 33, 2, 3];
        let routes = [VpnBypassRoute::default()];
        for _ in 0..100_000 {
            let mut buf = Vec::new();
            for _ in 0..(next() % 3 + 1) {
                let payload: Vec<u8> = (0..(next() % 96) as usize).map(|_| next() as u8).collect();
                // Mostly-honest length, sometimes lying, to hit both bounds paths.
                let len = if next() % 4 == 0 {
                    next() as u32 % 256
                } else {
                    16 + payload.len() as u32
                };
                buf.extend_from_slice(&len.to_ne_bytes());
                buf.extend_from_slice(&types[(next() % types.len() as u64) as usize].to_ne_bytes());
                buf.extend_from_slice(&[0u8; 10]);
                buf.extend_from_slice(&payload);
            }
            buf.truncate((next() as usize) % (buf.len() + 1));
            for msg in NetlinkMsgIter::new(&buf) {
                let mut rule_ctx = RuleDumpCtx::new();
                let _ = parse_rule_message(&msg, &routes, 12400, &mut rule_ctx);
                let _ = parse_rule_message(&msg, &[], 12400, &mut rule_ctx);
                let mut route_ctx = RouteFindCtx::default();
                let _ = parse_route_message(&msg, |_| Some("wlan0".to_string()), &mut route_ctx);
                let _ = parse_link_message(&msg);
                let _ = parse_xfrm_message(&msg);
                for _ in NetlinkAttrIter::new(msg.payload) {}
            }
            for _ in NetlinkAttrIter::new(&buf) {}
        }
    }
}
