//! Netlink and XFRM monitoring and VPN bypass rule management.
#![allow(unused_imports)]

pub mod bypass;
pub mod msg;
pub mod stats;
pub mod watcher;

#[cfg(test)]
mod tests;

pub use bypass::{
    drain_socket, if_indextoname_safe, netlink_rule_send, recv_all_with_timeout, vpn_bypass_sync,
    BypassManager,
};
pub use msg::{
    encode_getlink_stats_request, encode_link_dump_request, encode_route_dump_request,
    encode_rule_dump_request, encode_rule_request, netlink_add_attr, nlmsg_align, nlmsg_length,
    parse_link_message, parse_route_message, parse_rule_message, parse_xfrm_message, rta_align,
    rta_length, DumpedOursRule, LinkInfo, NetlinkAttr, NetlinkAttrIter, NetlinkMsg, NetlinkMsgIter,
    RouteFindCtx, RuleDumpCtx, XfrmEvent, AF_INET, AF_INET6, NETLINK_DEBOUNCE_MS, NETLINK_ROUTE,
    NETLINK_VPN_SETTLE_MS, NETLINK_XFRM, NLMSG_DONE, NLMSG_ERROR, RTMGRP_IPV4_IFADDR,
    RTMGRP_IPV4_ROUTE, RTMGRP_IPV4_RULE, RTMGRP_IPV6_IFADDR, RTMGRP_IPV6_ROUTE, RTMGRP_LINK,
    RTM_DELADDR, RTM_DELLINK, RTM_DELROUTE, RTM_DELRULE, RTM_GETADDR, RTM_GETLINK, RTM_GETROUTE,
    RTM_GETRULE, RTM_NEWADDR, RTM_NEWLINK, RTM_NEWROUTE, RTM_NEWRULE, RTNLGRP_IPV6_RULE,
    XFRMA_IF_ID, XFRMNLGRP_SA, XFRM_MSG_DELSA, XFRM_MSG_NEWSA,
};
pub use stats::{iface_stats, parse_proc_net_dev_line};
pub use watcher::{
    detect_active_vpn, detect_active_vpn_netlink, get_vpn_type_label, getifaddrs_find_vpn,
    is_proxy_interface, NetlinkWatcher,
};
