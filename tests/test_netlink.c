#include <stdio.h>
#include "netlink.h"

int main() {
    printf("=== Netlink Multi-VPN Test ===\n");
    
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
    
    return 0;
}
