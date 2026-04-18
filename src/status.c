#include "status.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include "service.h"
#include "api.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define TRAFFIC_STATE_FILE "/data/adb/atp/run/traffic.state"

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
    
    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) == 0 && vpn_iface[0]) {
        printf("VPN Status:\n");
        printf("    ├─ State:     CONNECTED\n");
        printf("    ├─ Interface: %s\n", vpn_iface);
        printf("    └─ IP:        (check with ip addr show %s)\n", vpn_iface);
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

/* Traffic monitoring structures */
typedef struct {
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    time_t timestamp;
    char iface[IFNAMSIZ];
} iface_stats_t;

/* Parse /proc/net/dev for a specific interface */
static int get_iface_traffic(const char *iface, unsigned long long *rx_bytes, unsigned long long *tx_bytes) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        return -1;
    }
    
    char line[512];
    int found = 0;
    
    /* Skip header lines (first two lines) */
    fgets(line, sizeof(line), fp);  /* "Inter-|..." */
    fgets(line, sizeof(line), fp);  /* " face |..." */
    
    while (fgets(line, sizeof(line), fp)) {
        char name[64];
        unsigned long long rx_bytes_val, tx_bytes_val;
        
        /* Parse: eth0: bytes packets errs drop fifo frame compressed multicast */
        if (sscanf(line, "%63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   name, &rx_bytes_val, &tx_bytes_val) >= 3) {
            /* Remove leading spaces from name */
            char *p = name;
            while (*p == ' ') p++;
            
            if (strcmp(p, iface) == 0) {
                *rx_bytes = rx_bytes_val;
                *tx_bytes = tx_bytes_val;
                found = 1;
                break;
            }
        }
    }
    
    fclose(fp);
    return found ? 0 : -1;
}

/* Format bytes to human readable string (KB, MB, GB) */
static void format_bytes(char *buf, size_t size, unsigned long long bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, size, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, size, "%.2f MB", (double)bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, size, "%.2f KB", (double)bytes / 1024);
    } else {
        snprintf(buf, size, "%llu B", bytes);
    }
}

/* Format speed to human readable string (Kbps, Mbps, Gbps) */
static void format_speed(char *buf, size_t size, unsigned long long bytes_per_sec) {
    unsigned long long bits_per_sec = bytes_per_sec * 8;
    
    if (bits_per_sec >= 1024 * 1024 * 1024) {
        snprintf(buf, size, "%.2f Gbps", (double)bits_per_sec / (1024 * 1024 * 1024));
    } else if (bits_per_sec >= 1024 * 1024) {
        snprintf(buf, size, "%.2f Mbps", (double)bits_per_sec / (1024 * 1024));
    } else if (bits_per_sec >= 1024) {
        snprintf(buf, size, "%.2f Kbps", (double)bits_per_sec / 1024);
    } else {
        snprintf(buf, size, "%llu bps", bits_per_sec);
    }
}

/* Load previous traffic stats from disk */
static int load_traffic_state(iface_stats_t *stats) {
    FILE *fp = fopen(TRAFFIC_STATE_FILE, "r");
    if (!fp) {
        return -1;
    }
    
    int ret = fscanf(fp, "%s %llu %llu %ld", 
                     stats->iface, 
                     &stats->rx_bytes, 
                     &stats->tx_bytes, 
                     (long*)&stats->timestamp);
    fclose(fp);
    
    if (ret == 4 && stats->iface[0] != '\0') {
        return 0;
    }
    return -1;
}

/* Save current traffic stats to disk */
static int save_traffic_state(const iface_stats_t *stats) {
    /* Ensure run directory exists */
    char dir[PATH_MAX];
    strncpy(dir, TRAFFIC_STATE_FILE, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_recursive(dir, 0755);
    }
    
    FILE *fp = fopen(TRAFFIC_STATE_FILE, "w");
    if (!fp) {
        LOG_DEBUG("Failed to save traffic state to %s", TRAFFIC_STATE_FILE);
        return -1;
    }
    
    fprintf(fp, "%s %llu %llu %ld\n", 
            stats->iface, 
            stats->rx_bytes, 
            stats->tx_bytes, 
            (long)stats->timestamp);
    fclose(fp);
    
    return 0;
}

/* Show real-time traffic for VPN interface with persistent state */
static void status_show_traffic(void) {
    char vpn_iface[IFNAMSIZ] = {0};
    unsigned long long rx_bytes, tx_bytes;
    char rx_str[32], tx_str[32];
    char rx_speed_str[32], tx_speed_str[32];
    iface_stats_t current_stats;
    iface_stats_t prev_stats;
    
    /* Detect active VPN interface */
    if (netlink_get_active_vpn(vpn_iface, sizeof(vpn_iface)) != 0 || vpn_iface[0] == '\0') {
        /* No VPN interface found, skip traffic monitoring */
        return;
    }
    
    /* Get current traffic stats */
    if (get_iface_traffic(vpn_iface, &rx_bytes, &tx_bytes) != 0) {
        LOG_DEBUG("Failed to read traffic stats for %s", vpn_iface);
        return;
    }
    
    /* Prepare current stats */
    memset(&current_stats, 0, sizeof(current_stats));
    strncpy(current_stats.iface, vpn_iface, IFNAMSIZ - 1);
    current_stats.rx_bytes = rx_bytes;
    current_stats.tx_bytes = tx_bytes;
    current_stats.timestamp = time(NULL);
    
    format_bytes(rx_str, sizeof(rx_str), rx_bytes);
    format_bytes(tx_str, sizeof(tx_str), tx_bytes);
    
    printf("VPN Traffic (%s):\n", vpn_iface);
    printf("    ├─ Total RX:  %s\n", rx_str);
    printf("    ├─ Total TX:  %s\n", tx_str);
    
    /* Try to load previous stats from disk */
    if (load_traffic_state(&prev_stats) == 0 && 
        strcmp(prev_stats.iface, vpn_iface) == 0 &&
        prev_stats.timestamp > 0) {
        
        double elapsed = difftime(current_stats.timestamp, prev_stats.timestamp);
        
        /* Only calculate speed if sample interval is reasonable (1-3600 seconds) */
        if (elapsed >= 1.0 && elapsed <= 3600.0) {
            unsigned long long rx_diff = (current_stats.rx_bytes > prev_stats.rx_bytes) ? 
                                         (current_stats.rx_bytes - prev_stats.rx_bytes) : 0;
            unsigned long long tx_diff = (current_stats.tx_bytes > prev_stats.tx_bytes) ? 
                                         (current_stats.tx_bytes - prev_stats.tx_bytes) : 0;
            
            unsigned long long rx_speed = (unsigned long long)((double)rx_diff / elapsed);
            unsigned long long tx_speed = (unsigned long long)((double)tx_diff / elapsed);
            
            format_speed(rx_speed_str, sizeof(rx_speed_str), rx_speed);
            format_speed(tx_speed_str, sizeof(tx_speed_str), tx_speed);
            
            printf("    ├─ Avg RX:   %s (over %.0fs)\n", rx_speed_str, elapsed);
            printf("    └─ Avg TX:   %s (over %.0fs)\n", tx_speed_str, elapsed);
        } else if (elapsed < 1.0) {
            printf("    ├─ Avg RX:   (sample too frequent, %.0fs)\n", elapsed);
            printf("    └─ Avg TX:   (sample too frequent, %.0fs)\n", elapsed);
        } else {
            printf("    ├─ Avg RX:   (sample too old, %.0fs)\n", elapsed);
            printf("    └─ Avg TX:   (sample too old, %.0fs)\n", elapsed);
        }
    } else {
        printf("    ├─ Avg RX:   (first sample, run status again for speed)\n");
        printf("    └─ Avg TX:   (first sample, run status again for speed)\n");
    }
    
    /* Save current stats for next time */
    save_traffic_state(&current_stats);
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
    
    status_show_traffic();
    printf("\n");
    
    status_show_network();
    printf("\n");
}
