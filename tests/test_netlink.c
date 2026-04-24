#include <stdio.h>
#include "netlink.h"

int main() {
    struct nl_rule *rules;
    int count;
    
    printf("=== Netlink Rule Test ===\n");
    
    if (nl_rule_list(&rules, &count) == 0) {
        printf("Found %d rules:\n", count);
        for (int i = 0; i < count; i++) {
            printf("  Rule %d: family=%d, table=%d, priority=%d, mark=0x%x\n",
                   i, rules[i].family, rules[i].table, 
                   rules[i].priority, rules[i].mark);
            if (rules[i].uid_range) {
                printf("    UID range: %u-%u\n", 
                       rules[i].uid_range->start, 
                       rules[i].uid_range->end);
            }
        }
        nl_rule_free(rules, count);
    }
    
    printf("\n=== VPN Detection ===\n");
    int vpn = nl_vpn_detect();
    printf("Android VPN enabled: %s\n", vpn ? "YES" : "NO");
    
    printf("\n=== Link Test ===\n");
    struct nl_link *links;
    int link_count;
    if (nl_link_list(&links, &link_count) == 0) {
        printf("Found %d links:\n", link_count);
        for (int i = 0; i < link_count; i++) {
            printf("  %s: index=%d, flags=0x%x, mtu=%d\n",
                   links[i].name, links[i].index, 
                   links[i].flags, links[i].mtu);
        }
        nl_link_free(links, link_count);
    }
    
    return 0;
}
