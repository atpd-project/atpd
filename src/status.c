#include "status.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include <stdio.h>

static const char* proxy_mode_to_string(atp_config_t *cfg) {
    if (cfg->proxy_mode == 0) {
        return cfg->use_tproxy ? "TPROXY (auto)" : "REDIRECT (auto)";
    } else if (cfg->proxy_mode == 1) {
        return "TPROXY";
    } else if (cfg->proxy_mode == 2) {
        return "REDIRECT";
    } else if (cfg->proxy_mode == 3) {
        return "ENHANCE (Split TCP:NAT / UDP:Mangle)";
    }
    return "UNKNOWN";
}

static void status_show_core(service_ctx_t *svc) {
    int pid = service_get_pid(svc);
    
    if (pid <= 0) {
        printf("sing-box service is stopped.\n");
        return;
    }
    
    long mem_kb = get_process_memory_kb(pid);
    double cpu = get_process_cpu_percent(pid);
    int threads = get_process_threads(pid);
    int fd_count = get_process_fd_count(pid);
    int uptime_sec = get_process_uptime_sec(pid);
    char uptime_str[64];
    char version[64];
    
    format_uptime(uptime_sec, uptime_str, sizeof(uptime_str));
    get_binary_version(PROXY_BIN_PATH, version, sizeof(version));
    
    printf("sing-box is running as root:net_admin.\n");
    printf("    ├─ PID:       %d\n", pid);
    printf("    ├─ Memory:    %ld kB\n", mem_kb);
    printf("    ├─ CPU:       %.1f%%\n", cpu);
    printf("    ├─ Threads:   %d\n", threads);
    printf("    ├─ Sockets:   %d (Active FDs)\n", fd_count);
    printf("    ├─ Uptime:    %s\n", uptime_str);
    printf("    └─ Version:   %s\n", version);
}

static void status_show_mode(atp_config_t *cfg, api_ctx_t *api) {
    char routing_mode[64] = {0};
    
    printf("Proxy Mode:\n");
    printf("    ├─ Traffic:   %s\n", proxy_mode_to_string(cfg));
    
    if (api_get_mode(api, routing_mode, sizeof(routing_mode)) == 0) {
        printf("    └─ Routing:   %s\n", routing_mode);
    } else {
        printf("    └─ Routing:   %s (cached)\n", cfg->user_clash_mode);
    }
}

static void status_show_vpn(void) {
    char vpn_iface[IFNAMSIZ] = {0};
    
    if (netlink_get_active_vpn(NULL, vpn_iface, sizeof(vpn_iface)) == 0) {
        netlink_iface_info_t info;
        printf("VPN Status:\n");
        printf("    ├─ State:     CONNECTED\n");
        printf("    ├─ Interface: %s\n", vpn_iface);
        
        if (netlink_get_iface_info(vpn_iface, &info) == 0 && info.has_ipv4) {
            printf("    └─ IP:        %s/%d\n", info.ipv4_addr, info.ipv4_prefix);
        } else {
            printf("    └─ IP:        (no IPv4 address)\n");
        }
    } else {
        printf("VPN Status:\n");
        printf("    └─ State:     DISCONNECTED\n");
    }
}

static void status_show_network(void) {
    char snapshot[1024] = {0};
    
    if (netlink_get_ipv4_snapshot(snapshot, sizeof(snapshot)) == 0 && snapshot[0] != '\0') {
        printf("Network Interfaces:\n");
        printf("    └─ Active:    %s\n", snapshot);
    } else {
        printf("Network Interfaces:\n");
        printf("    └─ Active:    (no active interfaces)\n");
    }
}

void status_show(atp_config_t *cfg, service_ctx_t *svc, api_ctx_t *api) {
    printf("\n=== ATP Status ===\n\n");
    
    printf("Proxy Core:\n");
    status_show_core(svc);
    printf("\n");
    
    status_show_mode(cfg, api);
    printf("\n");
    
    status_show_vpn();
    printf("\n");
    
    status_show_network();
    printf("\n");
}
