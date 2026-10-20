#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "netlink.h"

int main() {
    printf("=== Netlink Multi-VPN Test ===\n");

    assert(strcmp(netlink_get_vpn_type_label(NULL), "None") == 0);
    assert(strcmp(netlink_get_vpn_type_label(""), "None") == 0);
    assert(strcmp(netlink_get_vpn_type_label("tun0"), "Cloudflare WARP / TUN") == 0);
    assert(strcmp(netlink_get_vpn_type_label("warp0"), "Cloudflare WARP") == 0);
    assert(strcmp(netlink_get_vpn_type_label("wg0"), "WireGuard") == 0);
    assert(strcmp(netlink_get_vpn_type_label("tailscale0"), "Tailscale") == 0);
    assert(strcmp(netlink_get_vpn_type_label("zt0"), "ZeroTier") == 0);
    assert(strcmp(netlink_get_vpn_type_label("ipsec0"), "Google VPN / IPsec") == 0);
    assert(strcmp(netlink_get_vpn_type_label("xfrm0"), "IPsec XFRM") == 0);
    assert(strcmp(netlink_get_vpn_type_label("other"), "VPN Tunnel") == 0);
    
    char iface[IFNAMSIZ] = {0};
    int has_vpn = (netlink_get_active_vpn(iface, sizeof(iface)) == 0);
    printf("Active VPN Detected: %s\n", has_vpn ? "YES" : "NO");
    if (has_vpn) {
        printf("Interface: %s (%s)\n", iface, netlink_get_vpn_type_label(iface));
        uint64_t rx = 0, tx = 0;
        if (netlink_get_iface_stats(iface, &rx, &tx) == 0) {
            printf("Traffic: RX=%llu bytes, TX=%llu bytes\n", (unsigned long long)rx, (unsigned long long)tx);
        }
    }
    
    printf("=== Netlink Multi-VPN Test PASSED ===\n");
    return 0;
}
