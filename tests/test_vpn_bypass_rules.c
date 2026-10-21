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

static int matches(struct nlmsghdr *h) {
    struct rule_dump_ctx ctx = {0};
    CHECK(parser_rule_dump(h, &ctx) == 0);
    return ctx.count;
}

int main(void) {
    char buf[256];

    g_bypass_route_count = 1;
    g_bypass_routes[0].family = AF_INET;
    g_bypass_routes[0].prefix_len = 10;
    CHECK(inet_pton(AF_INET, "100.64.0.0", g_bypass_routes[0].addr) == 1);
    g_bypass_pref = 12400;

    /* Our own rule as the kernel dumps it: must be recognised so it can be removed. */
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 10, 1047, UINT32_MAX, 0)) == 1);
    /* netd-style rules and near misses must never be touched. */
    CHECK(matches(make_rule(buf, sizeof(buf), 16000, "100.64.0.0", 10, 1047, UINT32_MAX, 0)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 10, 1047, UINT32_MAX, 0x10070)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 10, 1047, 0, 0)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.64.0.0", 11, 1047, UINT32_MAX, 0)) == 0);
    CHECK(matches(make_rule(buf, sizeof(buf), 12400, "100.65.0.0", 10, 1047, UINT32_MAX, 0)) == 0);

    printf("VPN bypass rule matching tests passed\n");
    return 0;
}
