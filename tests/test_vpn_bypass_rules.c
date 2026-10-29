#include <stdio.h>
#include <stdlib.h>

#include "../src/netlink.c"

/* Not assert(): release test builds use -DNDEBUG. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

/* Builds an RTM_NEWRULE dump entry shaped like the kernel's (fib_nl_fill_rule). */
static struct nlmsghdr *make_rule(char *buf, size_t size, uint32_t pref,
                                  const char *dst, uint8_t dst_len, uint32_t table,
                                  uint32_t suppress_prefixlen, uint32_t fwmark) {
    memset(buf, 0, size);
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    h->nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    h->nlmsg_type = RTM_NEWRULE;
    struct fib_rule_hdr *frh = NLMSG_DATA(h);
    frh->family = AF_INET;
    frh->dst_len = dst_len;
    frh->action = FR_ACT_TO_TBL;
    frh->table = RT_TABLE_UNSPEC;

    uint8_t addr[4];
    CHECK(inet_pton(AF_INET, dst, addr) == 1);
    uint8_t proto = 0;
    CHECK(netlink_add_attr(h, size, FRA_DST, addr, sizeof(addr)) == 0);
    CHECK(netlink_add_attr(h, size, FRA_TABLE, &table, sizeof(table)) == 0);
    CHECK(netlink_add_attr(h, size, FRA_SUPPRESS_PREFIXLEN, &suppress_prefixlen, 4) == 0);
    CHECK(netlink_add_attr(h, size, FRA_PROTOCOL, &proto, sizeof(proto)) == 0);
    CHECK(netlink_add_attr(h, size, FRA_PRIORITY, &pref, sizeof(pref)) == 0);
    if (fwmark) CHECK(netlink_add_attr(h, size, FRA_FWMARK, &fwmark, sizeof(fwmark)) == 0);
    return h;
}

static struct nlmsghdr *make_rule_v6(char *buf, size_t size, uint32_t pref,
                                     const char *dst, uint8_t dst_len, uint32_t table,
                                     uint32_t suppress_prefixlen, uint32_t fwmark) {
    memset(buf, 0, size);
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    h->nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
    h->nlmsg_type = RTM_NEWRULE;
    struct fib_rule_hdr *frh = NLMSG_DATA(h);
    frh->family = AF_INET6;
    frh->dst_len = dst_len;
    frh->action = FR_ACT_TO_TBL;
    frh->table = RT_TABLE_UNSPEC;

    uint8_t addr[16];
    CHECK(inet_pton(AF_INET6, dst, addr) == 1);
    uint8_t proto = 0;
    CHECK(netlink_add_attr(h, size, FRA_DST, addr, sizeof(addr)) == 0);
    CHECK(netlink_add_attr(h, size, FRA_TABLE, &table, sizeof(table)) == 0);
    CHECK(netlink_add_attr(h, size, FRA_SUPPRESS_PREFIXLEN, &suppress_prefixlen, 4) == 0);
    CHECK(netlink_add_attr(h, size, FRA_PROTOCOL, &proto, sizeof(proto)) == 0);
    CHECK(netlink_add_attr(h, size, FRA_PRIORITY, &pref, sizeof(pref)) == 0);
    if (fwmark) CHECK(netlink_add_attr(h, size, FRA_FWMARK, &fwmark, sizeof(fwmark)) == 0);
    return h;
}

static struct nlmsghdr *make_route(char *buf, size_t size, int family,
                                   uint8_t dst_len, uint32_t table, int oif) {
    memset(buf, 0, size);
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    h->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    h->nlmsg_type = RTM_NEWROUTE;
    struct rtmsg *rtm = NLMSG_DATA(h);
    rtm->rtm_family = (unsigned char)family;
    rtm->rtm_dst_len = dst_len;
    rtm->rtm_table = RT_TABLE_UNSPEC;
    rtm->rtm_protocol = RTPROT_BOOT;
    rtm->rtm_scope = RT_SCOPE_UNIVERSE;
    rtm->rtm_type = RTN_UNICAST;

    CHECK(netlink_add_attr(h, size, RTA_TABLE, &table, sizeof(table)) == 0);
    CHECK(netlink_add_attr(h, size, RTA_OIF, &oif, sizeof(oif)) == 0);
    return h;
}

static int matches(struct nlmsghdr *h) {
    struct rule_dump_ctx ctx = {0};
    CHECK(parser_rule_dump(h, &ctx) == 0);
    return ctx.count;
}

int main(void) {
    char buf[256];

    g_bypass_route_count = 2;
    g_bypass_routes[0].family = AF_INET;
    g_bypass_routes[0].prefix_len = 10;
    CHECK(inet_pton(AF_INET, "100.64.0.0", g_bypass_routes[0].addr) == 1);
    g_bypass_routes[1].family = AF_INET6;
    g_bypass_routes[1].prefix_len = 48;
    CHECK(inet_pton(AF_INET6, "fd7a:115c:a1e0::", g_bypass_routes[1].addr) == 1);
    g_bypass_pref = 12400;

    /* Our own rule as the kernel dumps it: must be recognised so it can be removed. */
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 10, 1047, UINT32_MAX, 0)) == 1);
    /* netd-style rules and near misses must never be touched. */
    CHECK(matches(make_rule(buf, sizeof(buf), 16000, "100.64.0.0", 10, 1047, UINT32_MAX, 0)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 10, 1047, UINT32_MAX, 0x10070)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 10, 1047, 0, 0)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 11, 1047, UINT32_MAX, 0)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.65.0.0", 10, 1047, UINT32_MAX, 0)) == 0);

    /* IPv6 rule matching */
    CHECK(matches(make_rule_v6(buf, sizeof(buf), 12400, "fd7a:115c:a1e0::", 48, 1047, UINT32_MAX, 0)) == 1);
    CHECK(matches(make_rule_v6(buf, sizeof(buf), 16000, "fd7a:115c:a1e0::", 48, 1047, UINT32_MAX, 0)) == 0);
    CHECK(matches(make_rule_v6(buf, sizeof(buf), 12400, "fd7a:115c:a1e0::", 48, 1047, UINT32_MAX, 0x10070)) == 0);
    CHECK(matches(make_rule_v6(buf, sizeof(buf), 12400, "fd7a:115c:a1e0::", 49, 1047, UINT32_MAX, 0)) == 0);
    CHECK(matches(make_rule_v6(buf, sizeof(buf), 12400, "fd7a:115c:a1e1::", 48, 1047, UINT32_MAX, 0)) == 0);

    /* Test physical default route finding */
    int test_oif = 0;
    struct if_nameindex *if_list = if_nameindex();
    if (if_list) {
        for (struct if_nameindex *cur = if_list; cur->if_index && cur->if_name; cur++) {
            if (strcmp(cur->if_name, "lo") != 0 && !is_proxy_interface(cur->if_name)) {
                test_oif = (int)cur->if_index;
                break;
            }
        }
        if_freenameindex(if_list);
    }

    if (test_oif > 0) {
        /* IPv4 default route find */
        struct route_find_ctx r_ctx4 = {0};
        struct nlmsghdr *r4 = make_route(buf, sizeof(buf), AF_INET, 0, 1047, test_oif);
        CHECK(parser_route_find(r4, &r_ctx4) == 0);
        CHECK(r_ctx4.found_candidate == 1);
        CHECK(r_ctx4.selected_table == 1047);

        /* IPv6 default route find (must succeed on pure IPv6 network) */
        struct route_find_ctx r_ctx6 = {0};
        struct nlmsghdr *r6 = make_route(buf, sizeof(buf), AF_INET6, 0, 1048, test_oif);
        CHECK(parser_route_find(r6, &r_ctx6) == 0);
        CHECK(r_ctx6.found_candidate == 1);
        CHECK(r_ctx6.selected_table == 1048);
    }

    /* Test netlink_apply_vpn_bypass_config hot-reload */
    atp_config_t new_cfg;
    memset(&new_cfg, 0, sizeof(new_cfg));
    new_cfg.interface.vpn_bypass_pref = 12500;
    new_cfg.interface.vpn_bypass_route_count = 1;
    new_cfg.interface.vpn_bypass_routes[0].family = AF_INET;
    new_cfg.interface.vpn_bypass_routes[0].prefix_len = 8;
    CHECK(inet_pton(AF_INET, "10.0.0.0", new_cfg.interface.vpn_bypass_routes[0].addr) == 1);

    CHECK(netlink_apply_vpn_bypass_config(&new_cfg) == 0);
    CHECK(g_bypass_pref == 12500);
    CHECK(g_bypass_route_count == 1);
    CHECK(g_bypass_routes[0].prefix_len == 8);

    printf("VPN bypass rule matching tests passed\n");
    return 0;
}
