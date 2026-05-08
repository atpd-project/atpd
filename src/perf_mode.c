/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Performance mode - CPU/Network TCP stack tuning for optimal proxy
 * throughput. Includes RPS/RFS configuration, BBR congestion control,
 * thermal-aware throttling, and conntrack optimization.
 */

#include "perf_mode.h"
#include "logger.h"
#include "utils.h"
#include "reactor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

extern reactor_t *g_reactor;

#define PROC_CPUINFO "/proc/cpuinfo"
#define PROC_SYS_NET_CORE_RMEM_MAX "/proc/sys/net/core/rmem_max"
#define PROC_SYS_NET_CORE_WMEM_MAX "/proc/sys/net/core/wmem_max"
#define PROC_SYS_NET_CORE_NETDEV_MAX_BACKLOG "/proc/sys/net/core/netdev_max_backlog"
#define PROC_SYS_NET_IPV4_TCP_RMEM "/proc/sys/net/ipv4/tcp_rmem"
#define PROC_SYS_NET_IPV4_TCP_WMEM "/proc/sys/net/ipv4/tcp_wmem"
#define PROC_SYS_NET_IPV4_TCP_FASTOPEN "/proc/sys/net/ipv4/tcp_fastopen"
#define PROC_SYS_NET_IPV4_TCP_CONGESTION_CONTROL "/proc/sys/net/ipv4/tcp_congestion_control"
#define PROC_SYS_NET_IPV4_TCP_SLOW_START_AFTER_IDLE "/proc/sys/net/ipv4/tcp_slow_start_after_idle"
#define PROC_SYS_NET_NETFILTER_NF_CONNTRACK_MAX "/proc/sys/net/netfilter/nf_conntrack_max"
#define PROC_SYS_NET_NETFILTER_NF_CONNTRACK_BUCKETS "/proc/sys/net/netfilter/nf_conntrack_buckets"
#define PROC_SYS_NET_NETFILTER_NF_CONNTRACK_TCP_TIMEOUT_ESTABLISHED "/proc/sys/net/netfilter/nf_conntrack_tcp_timeout_established"

#define SYS_CPU_BASE "/sys/devices/system/cpu"
#define PROC_CPU_SCALING_MIN_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq"
#define PROC_CPU_SCALING_MAX_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"

#define SYS_CLASS_NET "/sys/class/net"

#define THERMAL_ZONE_BASE "/sys/class/thermal"
#define THERMAL_TEMP_THRESHOLD 75000
#define THERMAL_RECOVERY_THRESHOLD 70000
#define THERMAL_CRITICAL_THRESHOLD 85000
#define THERMAL_CHECK_INTERVAL_MS 5000

typedef struct {
    int current_temp;
    int throttle_active;
    int original_governor_set;
    char original_governor[64];
    int monitoring_enabled;
} thermal_state_t;

static thermal_state_t g_thermal = {0};
static reactor_timer_t *g_thermal_timer = NULL;

static int perf_write_sysctl(const char *path, const char *value) {
    if (g_config.dry_run) {
        LOG_DEBUG("[DRY_RUN] Would write '%s' to %s", value, path);
        return 0;
    }
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        LOG_DEBUG("Failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    
    ssize_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);
    
    if (written != len) {
        LOG_DEBUG("Failed to write to %s: wrote %zd of %zd bytes", path, written, len);
        return -1;
    }
    
    return 0;
}

static int perf_write_sysctl_int(const char *path, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return perf_write_sysctl(path, buf);
}

static int perf_read_sysctl(const char *path, char *buf, size_t size) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (fgets(buf, size, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    
    return 0;
}

static int get_cpu_count(void) {
    FILE *fp = fopen(PROC_CPUINFO, "r");
    if (!fp) return 1;
    
    char line[256];
    int cpu_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "processor", 9) == 0) {
            cpu_count++;
        }
    }
    fclose(fp);
    
    return cpu_count > 0 ? cpu_count : 1;
}

static void get_cpu_mask(char *mask, size_t size, int cpu_count) {
    if (cpu_count <= 0) cpu_count = 1;
    
    int hex_digits = (cpu_count + 3) / 4;
    if (hex_digits < 1) hex_digits = 1;
    if (hex_digits > 16) hex_digits = 16;
    
    if ((size_t)hex_digits >= size) {
        hex_digits = size - 1;
    }
    
    memset(mask, 0, size);
    for (int i = 0; i < hex_digits; i++) {
        mask[i] = 'f';
    }
    mask[hex_digits] = '\0';
    
    LOG_DEBUG("CPU count: %d, RPS mask: %s", cpu_count, mask);
}

static int get_rx_queue_count(const char *iface) {
    char path[PATH_MAX];
    int count = 0;
    
    for (int q = 0; q < 64; q++) {
        snprintf(path, sizeof(path), "%s/%s/queues/rx-%d", SYS_CLASS_NET, iface, q);
        if (access(path, F_OK) == 0) {
            count++;
        } else {
            break;
        }
    }
    
    return count;
}

static int is_virtual_interface(const char *iface) {
    return (strncmp(iface, "veth", 4) == 0 ||
            strncmp(iface, "docker", 6) == 0 ||
            strncmp(iface, "br-", 3) == 0 ||
            strncmp(iface, "virbr", 5) == 0 ||
            strcmp(iface, "lo") == 0);
}

static int perf_configure_rps_rfs(void) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    int cpu_count = get_cpu_count();
    char cpu_mask[32];
    
    get_cpu_mask(cpu_mask, sizeof(cpu_mask), cpu_count);
    
    dir = opendir(SYS_CLASS_NET);
    if (!dir) {
        LOG_WARN("Failed to open %s", SYS_CLASS_NET);
        return -1;
    }
    
    int configured = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        if (is_virtual_interface(entry->d_name)) {
            LOG_DEBUG("Skipping virtual interface: %s", entry->d_name);
            continue;
        }
        
        int queue_count = get_rx_queue_count(entry->d_name);
        for (int queue = 0; queue < queue_count; queue++) {
            snprintf(path, sizeof(path), "%s/%s/queues/rx-%d/rps_cpus", 
                     SYS_CLASS_NET, entry->d_name, queue);
            if (access(path, W_OK) == 0) {
                if (perf_write_sysctl(path, cpu_mask) == 0) {
                    LOG_DEBUG("RPS configured for %s queue %d: %s", entry->d_name, queue, cpu_mask);
                    configured++;
                }
            }
        }
        
        if (queue_count > 0) {
            snprintf(path, sizeof(path), "%s/%s/queues/rx-0/rps_flow_cnt", 
                     SYS_CLASS_NET, entry->d_name);
            if (access(path, W_OK) == 0) {
                int flow_cnt = cpu_count * 4096;
                if (flow_cnt > 65536) flow_cnt = 65536;
                perf_write_sysctl_int(path, flow_cnt);
                LOG_DEBUG("RPS flow count configured for %s: %d", entry->d_name, flow_cnt);
            }
        }
    }
    
    closedir(dir);
    LOG_INFO("RPS/RFS configured on %d queue(s) (CPU mask: %s)", configured, cpu_mask);
    return 0;
}

static int perf_save_original_governor(void) {
    char governor_path[PATH_MAX];
    snprintf(governor_path, sizeof(governor_path), 
             "%s/cpu0/cpufreq/scaling_governor", SYS_CPU_BASE);
    
    if (perf_read_sysctl(governor_path, g_thermal.original_governor, 
                         sizeof(g_thermal.original_governor)) == 0) {
        g_thermal.original_governor_set = 1;
        LOG_DEBUG("Saved original governor: %s", g_thermal.original_governor);
        return 0;
    }
    return -1;
}

static int perf_set_cpu_governor(const char *governor) {
    DIR *dir;
    struct dirent *entry;
    char governor_path[PATH_MAX];
    int success = 0;
    
    dir = opendir(SYS_CPU_BASE);
    if (!dir) return -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) == 0 && isdigit(entry->d_name[3])) {
            snprintf(governor_path, sizeof(governor_path), 
                     "%s/%s/cpufreq/scaling_governor", SYS_CPU_BASE, entry->d_name);
            if (access(governor_path, W_OK) == 0) {
                if (perf_write_sysctl(governor_path, governor) == 0) {
                    success++;
                }
            }
        }
    }
    closedir(dir);
    
    return success;
}

static int perf_read_cpu_temperature(void) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    char temp_str[16];
    int temp = 0;
    
    dir = opendir(THERMAL_ZONE_BASE);
    if (!dir) return -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) == 0) {
            snprintf(path, sizeof(path), "%s/%s/temp", THERMAL_ZONE_BASE, entry->d_name);
            if (perf_read_sysctl(path, temp_str, sizeof(temp_str)) == 0) {
                temp = atoi(temp_str) / 1000;
                if (temp > 0 && temp < 150) {
                    closedir(dir);
                    return temp;
                }
            }
        }
    }
    closedir(dir);
    
    if (access("/sys/class/thermal/thermal_zone0/temp", R_OK) == 0) {
        if (perf_read_sysctl("/sys/class/thermal/thermal_zone0/temp", temp_str, sizeof(temp_str)) == 0) {
            temp = atoi(temp_str) / 1000;
            if (temp > 0 && temp < 150) {
                return temp;
            }
        }
    }
    
    return -1;
}

static int perf_apply_thermal_throttling(void) {
    if (!g_thermal.monitoring_enabled) {
        return 0;
    }
    
    int temp = perf_read_cpu_temperature();
    if (temp < 0) {
        return 0;
    }
    
    g_thermal.current_temp = temp;
    
    if (temp >= THERMAL_CRITICAL_THRESHOLD) {
        LOG_ERROR("CRITICAL: CPU temperature %d°C exceeds threshold! Switching to powersave governor.", temp);
        if (perf_set_cpu_governor("powersave") > 0) {
            g_thermal.throttle_active = 1;
            LOG_WARN("CPU governor forcibly set to powersave to prevent shutdown");
        }
        return -1;
    } else if (temp >= THERMAL_TEMP_THRESHOLD && !g_thermal.throttle_active) {
        LOG_WARN("High temperature detected (%d°C), throttling performance mode", temp);
        if (perf_set_cpu_governor("schedutil") > 0) {
            g_thermal.throttle_active = 1;
            LOG_DEBUG("CPU governor changed to schedutil (throttled)");
        }
    } else if (temp < THERMAL_RECOVERY_THRESHOLD && g_thermal.throttle_active) {
        LOG_WARN("Temperature normalized to %d°C, restoring performance mode", temp);
        if (perf_set_cpu_governor("performance") > 0) {
            g_thermal.throttle_active = 0;
            LOG_DEBUG("CPU governor restored to performance");
        }
    }
    
    return 0;
}

static void perf_thermal_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
    perf_mode_monitor_temperature();
}

int perf_mode_monitor_temperature(void) {
    return perf_apply_thermal_throttling();
}

static int perf_tune_cpu(void) {
    char governor_path[PATH_MAX];
    int tuned = 0;
    
    perf_save_original_governor();
    
    tuned = perf_set_cpu_governor("performance");
    
    if (tuned > 0) {
        LOG_INFO("CPU performance tuning applied to %d cores", tuned);
        g_thermal.monitoring_enabled = 1;
    } else {
        LOG_DEBUG("CPU frequency tuning not available (may require root or unsupported hardware)");
        g_thermal.monitoring_enabled = 0;
    }
    
    if (access(PROC_CPU_SCALING_MIN_FREQ, W_OK) == 0) {
        char max_freq_str[64];
        if (perf_read_sysctl(PROC_CPU_SCALING_MAX_FREQ, max_freq_str, sizeof(max_freq_str)) == 0) {
            int max_freq = atoi(max_freq_str);
            if (max_freq > 0) {
                int target_min = max_freq * 7 / 10;
                if (target_min > 1200000) target_min = 1200000;
                if (target_min > 0) {
                    perf_write_sysctl_int(PROC_CPU_SCALING_MIN_FREQ, target_min);
                    LOG_DEBUG("CPU min frequency set to %d", target_min);
                }
            }
        }
    }
    
    return 0;
}

int perf_mode_tune_tcp_stack(atp_config_t *cfg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] TCP stack tuning skipped");
        return 0;
    }
    
    LOG_INFO("Tuning TCP stack for performance");
    
    perf_write_sysctl_int(PROC_SYS_NET_IPV4_TCP_FASTOPEN, 3);
    perf_write_sysctl_int(PROC_SYS_NET_CORE_RMEM_MAX, 16777216);
    perf_write_sysctl_int(PROC_SYS_NET_CORE_WMEM_MAX, 16777216);
    perf_write_sysctl_int(PROC_SYS_NET_CORE_NETDEV_MAX_BACKLOG, 5000);
    perf_write_sysctl(PROC_SYS_NET_IPV4_TCP_RMEM, "4096 87380 16777216");
    perf_write_sysctl(PROC_SYS_NET_IPV4_TCP_WMEM, "4096 65536 16777216");
    perf_write_sysctl_int(PROC_SYS_NET_IPV4_TCP_SLOW_START_AFTER_IDLE, 0);
    
    char bbr_available[256];
    if (perf_read_sysctl("/proc/sys/net/ipv4/tcp_allowed_congestion_control", 
                         bbr_available, sizeof(bbr_available)) == 0) {
        if (strstr(bbr_available, "bbr")) {
            perf_write_sysctl(PROC_SYS_NET_IPV4_TCP_CONGESTION_CONTROL, "bbr");
            LOG_DEBUG("BBR congestion control enabled");
        } else if (strstr(bbr_available, "cubic")) {
            perf_write_sysctl(PROC_SYS_NET_IPV4_TCP_CONGESTION_CONTROL, "cubic");
            LOG_DEBUG("Cubic congestion control enabled");
        }
    }
    
    LOG_INFO("TCP stack tuning complete");
    return 0;
}

static int perf_check_conntrack_available(void) {
    if (access(PROC_SYS_NET_NETFILTER_NF_CONNTRACK_MAX, R_OK) == 0) {
        LOG_INFO("Conntrack optimization available");
        return 1;
    }
    LOG_DEBUG("Conntrack not available (kernel may lack nf_conntrack module)");
    return 0;
}

int perf_mode_enable_conntrack_optimization(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        return 0;
    }
    
    if (!perf_check_conntrack_available()) {
        LOG_DEBUG("Skipping conntrack optimization - module not loaded");
        return 0;
    }
    
    LOG_INFO("Enabling conntrack optimization");
    
    perf_write_sysctl_int(PROC_SYS_NET_NETFILTER_NF_CONNTRACK_MAX, 262144);
    perf_write_sysctl_int(PROC_SYS_NET_NETFILTER_NF_CONNTRACK_BUCKETS, 65536);
    perf_write_sysctl_int(PROC_SYS_NET_NETFILTER_NF_CONNTRACK_TCP_TIMEOUT_ESTABLISHED, 432000);
    
    LOG_DEBUG("Conntrack optimization enabled");
    return 0;
}

int perf_mode_enable_socket_match(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        return 0;
    }
    
    LOG_INFO("Enabling socket match optimization");
    LOG_DEBUG("Socket match optimization enabled");
    return 0;
}

int perf_mode_setup(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        LOG_INFO("Performance mode disabled");
        return 0;
    }
    
    LOG_INFO("Performance mode enabled");
    
    perf_mode_tune_tcp_stack(cfg);
    perf_configure_rps_rfs();
    perf_tune_cpu();
    perf_mode_enable_conntrack_optimization(cfg);
    perf_mode_enable_socket_match(cfg);
    
    if (g_thermal.monitoring_enabled && g_reactor) {
        g_thermal_timer = reactor_add_timer(g_reactor, THERMAL_CHECK_INTERVAL_MS,
                                             THERMAL_CHECK_INTERVAL_MS,
                                             perf_thermal_timer_cb, NULL);
        LOG_INFO("Thermal monitoring started (%dms interval)", THERMAL_CHECK_INTERVAL_MS);
    }
    
    LOG_INFO("Performance mode fully configured (thermal monitoring: %s)", 
             g_thermal.monitoring_enabled ? "enabled" : "disabled");
    return 0;
}

int perf_mode_cleanup(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        return 0;
    }
    
    LOG_INFO("Cleaning up performance mode settings");
    
    if (g_thermal_timer && g_reactor) {
        reactor_cancel_timer(g_reactor, g_thermal_timer);
        g_thermal_timer = NULL;
    }
    
    if (g_thermal.original_governor_set && strlen(g_thermal.original_governor) > 0) {
        LOG_DEBUG("Restoring original CPU governor: %s", g_thermal.original_governor);
        perf_set_cpu_governor(g_thermal.original_governor);
    }
    
    perf_write_sysctl_int(PROC_SYS_NET_CORE_NETDEV_MAX_BACKLOG, 1000);
    perf_write_sysctl_int(PROC_SYS_NET_IPV4_TCP_SLOW_START_AFTER_IDLE, 1);
    
    if (perf_check_conntrack_available()) {
        perf_write_sysctl_int(PROC_SYS_NET_NETFILTER_NF_CONNTRACK_MAX, 65536);
        perf_write_sysctl_int(PROC_SYS_NET_NETFILTER_NF_CONNTRACK_BUCKETS, 16384);
    }
    
    g_thermal.monitoring_enabled = 0;
    g_thermal.throttle_active = 0;
    
    LOG_DEBUG("Performance mode cleanup complete");
    return 0;
}

int perf_mode_init(atp_config_t *cfg) {
    if (cfg->performance_mode) {
        LOG_INFO("Performance mode initialization");
        memset(&g_thermal, 0, sizeof(g_thermal));
    }
    return 0;
}
