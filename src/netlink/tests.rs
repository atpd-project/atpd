//! Tests for Netlink/XFRM VPN sensing and bypass rule synchronization.
//! Mirrored from tests/test_vpn_bypass_rules.c and tests/test_netlink.c.

use std::net::{Ipv4Addr, Ipv6Addr};
use std::str::FromStr;

use crate::config::{Config, VpnBypassRoute};
use crate::context::{AtpdContext, VpnState};
use crate::netlink::bypass::{if_indextoname_safe, BypassManager};
use crate::netlink::msg::{
    encode_rule_request, netlink_add_attr, nlmsg_length, parse_route_message, parse_rule_message,
    NetlinkMsgIter, RouteFindCtx, RuleDumpCtx, AF_INET, AF_INET6, FRA_DST, FRA_PRIORITY,
    FRA_PROTOCOL, FRA_SUPPRESS_PREFIXLEN, FRA_TABLE, FR_ACT_TO_TBL, RTA_OIF, RTA_TABLE,
    RTM_DELRULE, RTM_NEWLINK, RTM_NEWROUTE, RTM_NEWRULE, RT_TABLE_UNSPEC, XFRMA_IF_ID,
    XFRM_MSG_DELSA, XFRM_MSG_NEWSA,
};
use crate::netlink::stats::parse_proc_net_dev_line;
use crate::netlink::watcher::{is_proxy_interface, NetlinkWatcher};
use crate::reactor::{Event, Reactor};

const FRA_FWMARK: u16 = 10;

// Exact byte fixture for RTM_NEWRULE IPv4 (100.64.0.0/10, table 1047, pref 12400, seq 1) (52 bytes)
const RTM_NEWRULE_IPV4_BYTES: [u8; 52] = [
    0x34, 0x00, 0x00, 0x00, // nlmsg_len = 52
    0x20, 0x00, // nlmsg_type = RTM_NEWRULE (32)
    0x05,
    0x06, // nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL (0x0605)
    0x01, 0x00, 0x00, 0x00, // nlmsg_seq = 1
    0x00, 0x00, 0x00, 0x00, // nlmsg_pid = 0
    0x02, // family = AF_INET (2)
    0x0a, // dst_len = 10
    0x00, // src_len = 0
    0x00, // tos = 0
    0x00, // table = RT_TABLE_UNSPEC (0)
    0x00, // res1 = 0
    0x00, // res2 = 0
    0x01, // action = FR_ACT_TO_TBL (1)
    0x00, 0x00, 0x00, 0x00, // flags = 0
    0x08, 0x00, // rta_len = 8
    0x01, 0x00, // rta_type = FRA_DST (1)
    0x64, 0x40, 0x00, 0x00, // addr = 100.64.0.0
    0x08, 0x00, // rta_len = 8
    0x06, 0x00, // rta_type = FRA_PRIORITY (6)
    0x70, 0x30, 0x00, 0x00, // pref = 12400
    0x08, 0x00, // rta_len = 8
    0x0f, 0x00, // rta_type = FRA_TABLE (15)
    0x17, 0x04, 0x00, 0x00, // table = 1047
];

// Exact byte fixture for RTM_DELRULE IPv4 (100.64.0.0/10, table 1047, pref 12400, seq 1) (52 bytes)
const RTM_DELRULE_IPV4_BYTES: [u8; 52] = [
    0x34, 0x00, 0x00, 0x00, // nlmsg_len = 52
    0x21, 0x00, // nlmsg_type = RTM_DELRULE (33)
    0x05, 0x00, // nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK (0x0005)
    0x01, 0x00, 0x00, 0x00, // nlmsg_seq = 1
    0x00, 0x00, 0x00, 0x00, // nlmsg_pid = 0
    0x02, // family = AF_INET (2)
    0x0a, // dst_len = 10
    0x00, // src_len = 0
    0x00, // tos = 0
    0x00, // table = RT_TABLE_UNSPEC (0)
    0x00, // res1 = 0
    0x00, // res2 = 0
    0x01, // action = FR_ACT_TO_TBL (1)
    0x00, 0x00, 0x00, 0x00, // flags = 0
    0x08, 0x00, // rta_len = 8
    0x01, 0x00, // rta_type = FRA_DST (1)
    0x64, 0x40, 0x00, 0x00, // addr = 100.64.0.0
    0x08, 0x00, // rta_len = 8
    0x06, 0x00, // rta_type = FRA_PRIORITY (6)
    0x70, 0x30, 0x00, 0x00, // pref = 12400
    0x08, 0x00, // rta_len = 8
    0x0f, 0x00, // rta_type = FRA_TABLE (15)
    0x17, 0x04, 0x00, 0x00, // table = 1047
];

// Exact byte fixture for RTM_NEWRULE IPv6 (fd7a:115c:a1e0::/48, table 1048, pref 12400, seq 1) (64 bytes)
const RTM_NEWRULE_IPV6_BYTES: [u8; 64] = [
    0x40, 0x00, 0x00, 0x00, // nlmsg_len = 64
    0x20, 0x00, // nlmsg_type = RTM_NEWRULE (32)
    0x05, 0x06, // nlmsg_flags = 0x0605
    0x01, 0x00, 0x00, 0x00, // nlmsg_seq = 1
    0x00, 0x00, 0x00, 0x00, // nlmsg_pid = 0
    0x0a, // family = AF_INET6 (10)
    0x30, // dst_len = 48
    0x00, // src_len = 0
    0x00, // tos = 0
    0x00, // table = RT_TABLE_UNSPEC (0)
    0x00, // res1 = 0
    0x00, // res2 = 0
    0x01, // action = FR_ACT_TO_TBL (1)
    0x00, 0x00, 0x00, 0x00, // flags = 0
    0x14, 0x00, // rta_len = 20
    0x01, 0x00, // rta_type = FRA_DST (1)
    0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, // addr = fd7a:115c:a1e0::
    0x08, 0x00, // rta_len = 8
    0x06, 0x00, // rta_type = FRA_PRIORITY (6)
    0x70, 0x30, 0x00, 0x00, // pref = 12400
    0x08, 0x00, // rta_len = 8
    0x0f, 0x00, // rta_type = FRA_TABLE (15)
    0x18, 0x04, 0x00, 0x00, // table = 1048
];

// Exact byte fixture for RTM_DELRULE IPv6 (fd7a:115c:a1e0::/48, table 1048, pref 12400, seq 1) (64 bytes)
const RTM_DELRULE_IPV6_BYTES: [u8; 64] = [
    0x40, 0x00, 0x00, 0x00, // nlmsg_len = 64
    0x21, 0x00, // nlmsg_type = RTM_DELRULE (33)
    0x05, 0x00, // nlmsg_flags = 0x0005
    0x01, 0x00, 0x00, 0x00, // nlmsg_seq = 1
    0x00, 0x00, 0x00, 0x00, // nlmsg_pid = 0
    0x0a, // family = AF_INET6 (10)
    0x30, // dst_len = 48
    0x00, // src_len = 0
    0x00, // tos = 0
    0x00, // table = RT_TABLE_UNSPEC (0)
    0x00, // res1 = 0
    0x00, // res2 = 0
    0x01, // action = FR_ACT_TO_TBL (1)
    0x00, 0x00, 0x00, 0x00, // flags = 0
    0x14, 0x00, // rta_len = 20
    0x01, 0x00, // rta_type = FRA_DST (1)
    0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, // addr = fd7a:115c:a1e0::
    0x08, 0x00, // rta_len = 8
    0x06, 0x00, // rta_type = FRA_PRIORITY (6)
    0x70, 0x30, 0x00, 0x00, // pref = 12400
    0x08, 0x00, // rta_len = 8
    0x0f, 0x00, // rta_type = FRA_TABLE (15)
    0x18, 0x04, 0x00, 0x00, // table = 1048
];

/// Builds an RTM_NEWRULE dump entry shaped like the kernel's (fib_nl_fill_rule).
/// Mirrored from `make_rule` in tests/test_vpn_bypass_rules.c.
fn make_rule(
    pref: u32,
    dst: &str,
    dst_len: u8,
    table: u32,
    suppress_prefixlen: u32,
    fwmark: u32,
) -> Vec<u8> {
    let mut buf = Vec::with_capacity(128);
    let initial_len = nlmsg_length(12);
    buf.resize(initial_len, 0);

    // nlmsghdr
    buf[0..4].copy_from_slice(&(initial_len as u32).to_ne_bytes());
    buf[4..6].copy_from_slice(&RTM_NEWRULE.to_ne_bytes());
    buf[6..8].copy_from_slice(&0u16.to_ne_bytes());
    buf[8..12].copy_from_slice(&1u32.to_ne_bytes());
    buf[12..16].copy_from_slice(&0u32.to_ne_bytes());

    // fib_rule_hdr
    buf[16] = AF_INET as u8;
    buf[17] = dst_len;
    buf[18] = 0;
    buf[19] = 0;
    buf[20] = RT_TABLE_UNSPEC;
    buf[21] = 0;
    buf[22] = 0;
    buf[23] = FR_ACT_TO_TBL;
    buf[24..28].copy_from_slice(&0u32.to_ne_bytes());

    let addr = Ipv4Addr::from_str(dst).unwrap().octets();
    let proto = 0u8;

    netlink_add_attr(&mut buf, FRA_DST, &addr);
    netlink_add_attr(&mut buf, FRA_TABLE, &table.to_ne_bytes());
    netlink_add_attr(
        &mut buf,
        FRA_SUPPRESS_PREFIXLEN,
        &suppress_prefixlen.to_ne_bytes(),
    );
    netlink_add_attr(&mut buf, FRA_PROTOCOL, &[proto]);
    netlink_add_attr(&mut buf, FRA_PRIORITY, &pref.to_ne_bytes());
    if fwmark != 0 {
        netlink_add_attr(&mut buf, FRA_FWMARK, &fwmark.to_ne_bytes());
    }

    buf
}

/// Builds an RTM_NEWRULE IPv6 dump entry shaped like the kernel's.
/// Mirrored from `make_rule_v6` in tests/test_vpn_bypass_rules.c.
fn make_rule_v6(
    pref: u32,
    dst: &str,
    dst_len: u8,
    table: u32,
    suppress_prefixlen: u32,
    fwmark: u32,
) -> Vec<u8> {
    let mut buf = Vec::with_capacity(128);
    let initial_len = nlmsg_length(12);
    buf.resize(initial_len, 0);

    buf[0..4].copy_from_slice(&(initial_len as u32).to_ne_bytes());
    buf[4..6].copy_from_slice(&RTM_NEWRULE.to_ne_bytes());
    buf[6..8].copy_from_slice(&0u16.to_ne_bytes());
    buf[8..12].copy_from_slice(&1u32.to_ne_bytes());
    buf[12..16].copy_from_slice(&0u32.to_ne_bytes());

    buf[16] = AF_INET6 as u8;
    buf[17] = dst_len;
    buf[18] = 0;
    buf[19] = 0;
    buf[20] = RT_TABLE_UNSPEC;
    buf[21] = 0;
    buf[22] = 0;
    buf[23] = FR_ACT_TO_TBL;
    buf[24..28].copy_from_slice(&0u32.to_ne_bytes());

    let addr = Ipv6Addr::from_str(dst).unwrap().octets();
    let proto = 0u8;

    netlink_add_attr(&mut buf, FRA_DST, &addr);
    netlink_add_attr(&mut buf, FRA_TABLE, &table.to_ne_bytes());
    netlink_add_attr(
        &mut buf,
        FRA_SUPPRESS_PREFIXLEN,
        &suppress_prefixlen.to_ne_bytes(),
    );
    netlink_add_attr(&mut buf, FRA_PROTOCOL, &[proto]);
    netlink_add_attr(&mut buf, FRA_PRIORITY, &pref.to_ne_bytes());
    if fwmark != 0 {
        netlink_add_attr(&mut buf, FRA_FWMARK, &fwmark.to_ne_bytes());
    }

    buf
}

/// Builds an RTM_NEWROUTE entry shaped like the kernel's default route.
/// Mirrored from `make_route` in tests/test_vpn_bypass_rules.c.
fn make_route(family: i32, dst_len: u8, table: u32, oif: i32) -> Vec<u8> {
    let mut buf = Vec::with_capacity(128);
    let initial_len = nlmsg_length(12);
    buf.resize(initial_len, 0);

    buf[0..4].copy_from_slice(&(initial_len as u32).to_ne_bytes());
    buf[4..6].copy_from_slice(&RTM_NEWROUTE.to_ne_bytes());

    buf[16] = family as u8;
    buf[17] = dst_len;
    buf[20] = RT_TABLE_UNSPEC;

    netlink_add_attr(&mut buf, RTA_TABLE, &table.to_ne_bytes());
    netlink_add_attr(&mut buf, RTA_OIF, &oif.to_ne_bytes());

    buf
}

fn matches(buf: &[u8], routes: &[VpnBypassRoute], pref: u32) -> usize {
    let mut ctx = RuleDumpCtx::new();
    for msg in NetlinkMsgIter::new(buf) {
        let _ = parse_rule_message(&msg, routes, pref, &mut ctx);
    }
    ctx.rules.len()
}

// ---------------------------------------------------------------------------
// Mirrored tests
// ---------------------------------------------------------------------------

#[test]
fn test_vpn_bypass_rule_matching_mirrored() {
    // Mirrored from tests/test_vpn_bypass_rules.c: main() rule matching
    let mut routes = Vec::new();

    let mut addr1 = [0u8; 16];
    addr1[..4].copy_from_slice(&Ipv4Addr::from_str("100.64.0.0").unwrap().octets());
    routes.push(VpnBypassRoute {
        family: AF_INET,
        addr: addr1,
        prefix_len: 10,
    });

    let mut addr2 = [0u8; 16];
    addr2[..16].copy_from_slice(&Ipv6Addr::from_str("fd7a:115c:a1e0::").unwrap().octets());
    routes.push(VpnBypassRoute {
        family: AF_INET6,
        addr: addr2,
        prefix_len: 48,
    });

    let pref = 12400;

    // Our own rule as the kernel dumps it: must be recognised so it can be removed.
    let r_valid = make_rule(12400, "100.64.0.0", 10, 1047, u32::MAX, 0);
    assert_eq!(matches(&r_valid, &routes, pref), 1);

    // netd-style rules and near misses must never be touched.
    assert_eq!(
        matches(
            &make_rule(16000, "100.64.0.0", 10, 1047, u32::MAX, 0),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule(12400, "100.64.0.0", 10, 1047, u32::MAX, 0x10070),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule(12400, "100.64.0.0", 10, 1047, 0, 0),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule(12400, "100.64.0.0", 11, 1047, u32::MAX, 0),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule(12400, "100.65.0.0", 10, 1047, u32::MAX, 0),
            &routes,
            pref
        ),
        0
    );

    // IPv6 rule matching
    let r_v6_valid = make_rule_v6(12400, "fd7a:115c:a1e0::", 48, 1047, u32::MAX, 0);
    assert_eq!(matches(&r_v6_valid, &routes, pref), 1);
    assert_eq!(
        matches(
            &make_rule_v6(16000, "fd7a:115c:a1e0::", 48, 1047, u32::MAX, 0),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule_v6(12400, "fd7a:115c:a1e0::", 48, 1047, u32::MAX, 0x10070),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule_v6(12400, "fd7a:115c:a1e0::", 49, 1047, u32::MAX, 0),
            &routes,
            pref
        ),
        0
    );
    assert_eq!(
        matches(
            &make_rule_v6(12400, "fd7a:115c:a1e1::", 48, 1047, u32::MAX, 0),
            &routes,
            pref
        ),
        0
    );
}

#[test]
fn test_vpn_bypass_default_route_finding_mirrored() {
    // Mirrored from tests/test_vpn_bypass_rules.c: physical default route finding
    let mut test_oif = 0i32;

    // SAFETY: if_nameindex returns a pointer to an array of if_nameindex structs.
    let if_list = unsafe { libc::if_nameindex() };
    if !if_list.is_null() {
        let mut cur = if_list;
        loop {
            // SAFETY: cur is a non-null pointer within the array terminated by if_index == 0.
            let item = unsafe { *cur };
            if item.if_index == 0 || item.if_name.is_null() {
                break;
            }
            // SAFETY: item.if_name is a valid C string.
            let name = unsafe { std::ffi::CStr::from_ptr(item.if_name) }
                .to_str()
                .unwrap_or("");
            if name != "lo" && !is_proxy_interface(name) {
                test_oif = item.if_index as i32;
                break;
            }
            // SAFETY: advance pointer to next if_nameindex entry.
            cur = unsafe { cur.add(1) };
        }
        // SAFETY: free memory allocated by if_nameindex.
        unsafe { libc::if_freenameindex(if_list) };
    }

    if test_oif > 0 {
        // IPv4 default route find
        let r4 = make_route(AF_INET, 0, 1047, test_oif);
        let mut r_ctx4 = RouteFindCtx::default();
        for msg in NetlinkMsgIter::new(&r4) {
            parse_route_message(&msg, if_indextoname_safe, &mut r_ctx4);
        }
        assert!(r_ctx4.found_candidate);
        assert_eq!(r_ctx4.selected_table, 1047);

        // IPv6 default route find (must succeed on pure IPv6 network)
        let r6 = make_route(AF_INET6, 0, 1048, test_oif);
        let mut r_ctx6 = RouteFindCtx::default();
        for msg in NetlinkMsgIter::new(&r6) {
            parse_route_message(&msg, if_indextoname_safe, &mut r_ctx6);
        }
        assert!(r_ctx6.found_candidate);
        assert_eq!(r_ctx6.selected_table, 1048);
    }
}

#[test]
fn test_vpn_bypass_config_hot_reload_mirrored() {
    // Mirrored from tests/test_vpn_bypass_rules.c: netlink_apply_vpn_bypass_config hot-reload
    let mut bypass = BypassManager::new();

    let mut new_cfg = Config::default();
    new_cfg.interface.vpn_bypass_pref = 12500;
    new_cfg.interface.vpn_bypass_route_count = 1;
    new_cfg.interface.vpn_bypass_routes[0].family = AF_INET;
    new_cfg.interface.vpn_bypass_routes[0].prefix_len = 8;
    new_cfg.interface.vpn_bypass_routes[0].addr[..4]
        .copy_from_slice(&Ipv4Addr::from_str("10.0.0.0").unwrap().octets());

    bypass.apply_config(&new_cfg, None, None);
    assert_eq!(bypass.pref, 12500);
    assert_eq!(bypass.routes.len(), 1);
    assert_eq!(bypass.routes[0].prefix_len, 8);

    // Test removing multiple routes with unchanged priority
    let mut multi_cfg = Config::default();
    multi_cfg.interface.vpn_bypass_pref = 12500;
    multi_cfg.interface.vpn_bypass_route_count = 3;
    multi_cfg.interface.vpn_bypass_routes[0].family = AF_INET;
    multi_cfg.interface.vpn_bypass_routes[0].prefix_len = 8;
    multi_cfg.interface.vpn_bypass_routes[0].addr[..4]
        .copy_from_slice(&Ipv4Addr::from_str("10.0.0.0").unwrap().octets());
    multi_cfg.interface.vpn_bypass_routes[1].family = AF_INET;
    multi_cfg.interface.vpn_bypass_routes[1].prefix_len = 12;
    multi_cfg.interface.vpn_bypass_routes[1].addr[..4]
        .copy_from_slice(&Ipv4Addr::from_str("172.16.0.0").unwrap().octets());
    multi_cfg.interface.vpn_bypass_routes[2].family = AF_INET;
    multi_cfg.interface.vpn_bypass_routes[2].prefix_len = 16;
    multi_cfg.interface.vpn_bypass_routes[2].addr[..4]
        .copy_from_slice(&Ipv4Addr::from_str("192.168.0.0").unwrap().octets());

    bypass.apply_config(&multi_cfg, None, None);
    assert_eq!(bypass.routes.len(), 3);

    // Now update to keep ONLY 10.0.0.0/8 (removing 2 routes: 172.16.0.0/12 and 192.168.0.0/16)
    let mut single_cfg = Config::default();
    single_cfg.interface.vpn_bypass_pref = 12500;
    single_cfg.interface.vpn_bypass_route_count = 1;
    single_cfg.interface.vpn_bypass_routes[0].family = AF_INET;
    single_cfg.interface.vpn_bypass_routes[0].prefix_len = 8;
    single_cfg.interface.vpn_bypass_routes[0].addr[..4]
        .copy_from_slice(&Ipv4Addr::from_str("10.0.0.0").unwrap().octets());

    bypass.apply_config(&single_cfg, None, None);
    assert_eq!(bypass.routes.len(), 1);
    assert_eq!(bypass.routes[0].prefix_len, 8);
}

#[test]
fn test_netlink_proxy_interface_mirrored() {
    // Proxy interface check
    assert!(is_proxy_interface("ipsec0"));
    assert!(is_proxy_interface("xfrm0"));
    assert!(is_proxy_interface("tun0"));
    assert!(is_proxy_interface("tap0"));
    assert!(is_proxy_interface("warp0"));
    assert!(is_proxy_interface("wg0"));
    assert!(is_proxy_interface("tailscale0"));
    assert!(is_proxy_interface("zt0"));
    assert!(is_proxy_interface("zerotier0"));
    assert!(is_proxy_interface("utun0"));
    assert!(is_proxy_interface("vpn0"));
    assert!(is_proxy_interface("ppp0"));

    assert!(!is_proxy_interface("eth0"));
    assert!(!is_proxy_interface("wlan0"));
    assert!(!is_proxy_interface("lo"));
    assert!(!is_proxy_interface("enp0s3"));
}

// ---------------------------------------------------------------------------
// Byte fixture tests
// ---------------------------------------------------------------------------

#[test]
fn test_byte_fixture_rtm_newrule_ipv4_pref12400() {
    let addr = Ipv4Addr::from_str("100.64.0.0").unwrap().octets();
    let encoded = encode_rule_request(RTM_NEWRULE, AF_INET, 10, &addr, 1047, 12400, 1);

    assert_eq!(encoded.as_slice(), &RTM_NEWRULE_IPV4_BYTES[..]);

    // Test decoding it back
    let mut addr_buf = [0u8; 16];
    addr_buf[..4].copy_from_slice(&addr);
    let routes = vec![VpnBypassRoute {
        family: AF_INET,
        addr: addr_buf,
        prefix_len: 10,
    }];

    let mut ctx = RuleDumpCtx::new();
    for msg in NetlinkMsgIter::new(&encoded) {
        let matched = parse_rule_message(&msg, &routes, 12400, &mut ctx);
        assert!(matched);
    }
    assert_eq!(ctx.rules.len(), 1);
    assert_eq!(ctx.rules[0].table, 1047);
    assert_eq!(ctx.rules[0].family, AF_INET);
    assert_eq!(ctx.rules[0].dst_len, 10);
    assert_eq!(&ctx.rules[0].addr[..4], &addr);
}

#[test]
fn test_byte_fixture_rtm_delrule_ipv4_pref12400() {
    let addr = Ipv4Addr::from_str("100.64.0.0").unwrap().octets();
    let encoded = encode_rule_request(RTM_DELRULE, AF_INET, 10, &addr, 1047, 12400, 1);

    assert_eq!(encoded.as_slice(), &RTM_DELRULE_IPV4_BYTES[..]);
}

#[test]
fn test_byte_fixture_rtm_newrule_ipv6_pref12400() {
    let addr = Ipv6Addr::from_str("fd7a:115c:a1e0::").unwrap().octets();
    let encoded = encode_rule_request(RTM_NEWRULE, AF_INET6, 48, &addr, 1048, 12400, 1);

    assert_eq!(encoded.as_slice(), &RTM_NEWRULE_IPV6_BYTES[..]);

    let routes = vec![VpnBypassRoute {
        family: AF_INET6,
        addr,
        prefix_len: 48,
    }];

    let mut ctx = RuleDumpCtx::new();
    for msg in NetlinkMsgIter::new(&encoded) {
        let matched = parse_rule_message(&msg, &routes, 12400, &mut ctx);
        assert!(matched);
    }
    assert_eq!(ctx.rules.len(), 1);
    assert_eq!(ctx.rules[0].table, 1048);
    assert_eq!(ctx.rules[0].family, AF_INET6);
    assert_eq!(ctx.rules[0].dst_len, 48);
    assert_eq!(&ctx.rules[0].addr[..16], &addr);
}

#[test]
fn test_byte_fixture_rtm_delrule_ipv6_pref12400() {
    let addr = Ipv6Addr::from_str("fd7a:115c:a1e0::").unwrap().octets();
    let encoded = encode_rule_request(RTM_DELRULE, AF_INET6, 48, &addr, 1048, 12400, 1);

    assert_eq!(encoded.as_slice(), &RTM_DELRULE_IPV6_BYTES[..]);
}

// ---------------------------------------------------------------------------
// /proc/net/dev pure parser and stats tests
// ---------------------------------------------------------------------------

#[test]
fn test_proc_net_dev_pure_line_parser() {
    let line1 = "    lo: 546600931  265117    0    0    0     0          0         0 546600931  265117    0    0    0     0       0          0";
    let parsed1 = parse_proc_net_dev_line(line1);
    assert_eq!(parsed1, Some(("lo", 546600931, 546600931)));

    let line2 = "enp0s3: 6120748831 11859723    1    2    3     4          5         6 3274108272 12397703    0    0    0     0       0          0";
    let parsed2 = parse_proc_net_dev_line(line2);
    assert_eq!(parsed2, Some(("enp0s3", 6120748831, 3274108272)));

    // Header lines
    assert_eq!(
        parse_proc_net_dev_line(
            "Inter-|   Receive                                                |  Transmit"
        ),
        None
    );
    assert_eq!(
        parse_proc_net_dev_line(
            " face |bytes    packets errs drop fifo frame compressed multicast|bytes"
        ),
        None
    );
    // Invalid / empty
    assert_eq!(parse_proc_net_dev_line(""), None);
    assert_eq!(parse_proc_net_dev_line("invalid_no_colon"), None);
    assert_eq!(parse_proc_net_dev_line("  : 1 2 3 4 5 6 7 8 9"), None);
    assert_eq!(parse_proc_net_dev_line("eth0: 1 2 3"), None);
}

// ---------------------------------------------------------------------------
// Non-root multicast socket test
// ---------------------------------------------------------------------------

#[test]
fn test_non_root_multicast_socket_and_drain() {
    let mut reactor = Reactor::new().unwrap();
    let watcher = NetlinkWatcher::new(&mut reactor, 100, 101);
    assert!(
        watcher.is_ok(),
        "NETLINK_ROUTE multicast socket must open without root"
    );
    let mut w = watcher.unwrap();

    // Bounded wait: register a 10ms timer to ensure wait returns promptly
    let guard_timer = reactor.add_timer(10, 0);
    let mut events = Vec::new();
    reactor.wait(&mut events).unwrap();

    for ev in events {
        if let Event::Io { token, .. } = ev {
            let mut ctx = AtpdContext::new();
            ctx.init().unwrap();
            let cfg = Config::default();
            let changes = w.handle_io(&mut reactor, &mut ctx, &cfg, token);
            assert!(changes.is_empty());
        }
    }
    reactor.cancel_timer(guard_timer);
}

// ---------------------------------------------------------------------------
// Debounce burst test: 30 events produce exactly 1 refresh after debounce window
// ---------------------------------------------------------------------------

#[test]
fn test_debounce_burst_produces_single_refresh() {
    let mut reactor = Reactor::new().unwrap();
    let mut watcher = NetlinkWatcher::new(&mut reactor, 200, 201).unwrap();
    let mut ctx = AtpdContext::new();
    ctx.init().unwrap();
    let cfg = Config::default();

    // Create synthetic RTM_NEWLINK message
    let mut synthetic_link_event = vec![0u8; 32];
    synthetic_link_event[0..4].copy_from_slice(&32u32.to_ne_bytes());
    synthetic_link_event[4..6].copy_from_slice(&RTM_NEWLINK.to_ne_bytes());

    // Send a burst of 30 synthetic events through the parse path
    for _ in 0..30 {
        watcher.handle_route_bytes(&mut reactor, &synthetic_link_event);
    }

    assert!(watcher.debounce_timer.is_some());

    // Add a bounded safety timeout timer (1500 ms) so wait can never hang
    let safety_timer = reactor.add_timer(1500, 0);

    let mut timer_fired_count = 0;
    let mut events = Vec::new();

    while timer_fired_count == 0 {
        events.clear();
        reactor.wait(&mut events).unwrap();

        for ev in &events {
            if let Event::Timer(id) = *ev {
                if id == safety_timer {
                    panic!("Test timed out waiting for debounce timer");
                }
                if let Some(_changes) = watcher.on_timer(&mut reactor, &mut ctx, &cfg, id) {
                    timer_fired_count += 1;
                }
            }
        }
    }

    reactor.cancel_timer(safety_timer);

    // Verify debounce timer is now cleared
    assert!(watcher.debounce_timer.is_none());
    assert_eq!(
        timer_fired_count, 1,
        "Burst of 30 events must produce exactly one debounce refresh"
    );
}

// ---------------------------------------------------------------------------
// Synthetic XFRM SA Add / Del events test
// ---------------------------------------------------------------------------

#[test]
fn test_xfrm_sa_add_and_del_events() {
    let mut reactor = Reactor::new().unwrap();
    let mut watcher = NetlinkWatcher::new(&mut reactor, 300, 301).unwrap();
    let mut ctx = AtpdContext::new();
    ctx.init().unwrap();

    // Construct synthetic XFRM_MSG_NEWSA with XFRMA_IF_ID = 2
    let mut newsa_buf = Vec::new();
    let initial_len = nlmsg_length(224); // header (16) + xfrm_usersa_info (224) = 240
    newsa_buf.resize(initial_len, 0);

    newsa_buf[0..4].copy_from_slice(&(initial_len as u32).to_ne_bytes());
    newsa_buf[4..6].copy_from_slice(&XFRM_MSG_NEWSA.to_ne_bytes());
    newsa_buf[8..12].copy_from_slice(&1u32.to_ne_bytes());

    let if_id: u32 = 2;
    netlink_add_attr(&mut newsa_buf, XFRMA_IF_ID, &if_id.to_ne_bytes());

    let changes = watcher.handle_xfrm_bytes(&mut reactor, &mut ctx, &newsa_buf);
    assert_eq!(changes.len(), 1);
    assert_eq!(changes[0].state, VpnState::Predicting);
    assert_eq!(changes[0].iface.as_deref(), Some("ipsec1"));

    // Construct synthetic XFRM_MSG_DELSA
    let mut delsa_buf = vec![0u8; 16];
    delsa_buf[0..4].copy_from_slice(&16u32.to_ne_bytes());
    delsa_buf[4..6].copy_from_slice(&XFRM_MSG_DELSA.to_ne_bytes());
    delsa_buf[8..12].copy_from_slice(&2u32.to_ne_bytes());

    let changes_del = watcher.handle_xfrm_bytes(&mut reactor, &mut ctx, &delsa_buf);
    assert_eq!(changes_del.len(), 1);
    assert_eq!(changes_del[0].state, VpnState::Teardown);
    assert_eq!(changes_del[0].iface, None);

    // Regression: NEWSA followed by DELSA in one recv buffer must process both messages
    // (C's `break` only leaves the attribute loop).
    let mut ctx2 = AtpdContext::new();
    ctx2.init().unwrap();
    let mut both = newsa_buf.clone();
    both.extend_from_slice(&delsa_buf);
    let changes_both = watcher.handle_xfrm_bytes(&mut reactor, &mut ctx2, &both);
    let states: Vec<VpnState> = changes_both.iter().map(|c| c.state).collect();
    assert_eq!(states, vec![VpnState::Predicting, VpnState::Teardown]);
}

// ---------------------------------------------------------------------------
// CAP_NET_ADMIN test that skips cleanly when not root
// ---------------------------------------------------------------------------

#[test]
fn test_cap_net_admin_operations_skip_when_non_root() {
    // SAFETY: geteuid has no preconditions and is always safe.
    let euid = unsafe { libc::geteuid() };
    if euid != 0 {
        eprintln!("Skipping test_cap_net_admin_operations: requires root (CAP_NET_ADMIN)");
        return;
    }

    // If running as root (e.g. in privileged container or CI), test full rule sync
    let mut bypass = BypassManager::new();
    let mut cfg = Config::default();
    cfg.interface.vpn_bypass_pref = 12450;
    cfg.interface.vpn_bypass_route_count = 1;
    cfg.interface.vpn_bypass_routes[0].family = AF_INET;
    cfg.interface.vpn_bypass_routes[0].prefix_len = 32;
    cfg.interface.vpn_bypass_routes[0].addr[..4]
        .copy_from_slice(&Ipv4Addr::from_str("198.51.100.1").unwrap().octets());

    let mut reactor = Reactor::new().unwrap();
    let watcher = NetlinkWatcher::new(&mut reactor, 400, 401).unwrap();
    let sync_fd = watcher
        .sync_fd
        .as_ref()
        .map(std::os::unix::io::AsRawFd::as_raw_fd);

    bypass.apply_config(&cfg, sync_fd, Some("ipsec0"));

    // Cleanup rule
    cfg.interface.vpn_bypass_route_count = 0;
    bypass.apply_config(&cfg, sync_fd, None);
}
