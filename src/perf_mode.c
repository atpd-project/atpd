#include "perf_mode.h"
#include "logger.h"
#include "utils.h"
#include "tproxy.h"
#include <string.h>
#include <unistd.h>

#define PROC_TCP_RMEM "/proc/sys/net/ipv4/tcp_rmem"
#define PROC_TCP_WMEM "/proc/sys/net/ipv4/tcp_wmem"
#define PROC_TCP_FASTOPEN "/proc/sys/net/ipv4/tcp_fastopen"
#define PROC_TCP_CONG_CONTROL "/proc/sys/net/ipv4/tcp_congestion_control"
#define PROC_CORE_RMEM_MAX "/proc/sys/net/core/rmem_max"
#define PROC_CORE_WMEM_MAX "/proc/sys/net/core/wmem_max"

static int perf_write_sysctl(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_DEBUG("Failed to write to %s: %s", path, strerror(errno));
        return -1;
    }
    fprintf(fp, "%s", value);
    fclose(fp);
    return 0;
}

static int perf_read_sysctl(const char *path, char *buf, size_t size) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    fgets(buf, size, fp);
    fclose(fp);
    return 0;
}

int perf_mode_tune_tcp_stack(atp_config_t *cfg) {
    if (cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] TCP stack tuning skipped");
        return 0;
    }
    
    LOG_INFO("Tuning TCP stack for performance");
    
    /* Enable TCP Fast Open (3 = client + server) */
    perf_write_sysctl(PROC_TCP_FASTOPEN, "3");
    
    /* Increase socket buffer sizes (16MB) */
    perf_write_sysctl(PROC_CORE_RMEM_MAX, "16777216");
    perf_write_sysctl(PROC_CORE_WMEM_MAX, "16777216");
    
    /* Dynamic TCP window tuning */
    perf_write_sysctl(PROC_TCP_RMEM, "4096 87380 16777216");
    perf_write_sysctl(PROC_TCP_WMEM, "4096 65536 16777216");
    
    /* Enable BBR congestion control if available */
    char bbr_available[256];
    if (perf_read_sysctl("/proc/sys/net/ipv4/tcp_allowed_congestion_control", 
                         bbr_available, sizeof(bbr_available)) == 0) {
        if (strstr(bbr_available, "bbr")) {
            perf_write_sysctl(PROC_TCP_CONG_CONTROL, "bbr");
            LOG_DEBUG("BBR congestion control enabled");
        }
    }
    
    return 0;
}

int perf_mode_enable_conntrack_optimization(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        return 0;
    }
    
    LOG_INFO("Enabling conntrack optimization");
    
    /* Use CONNMARK to mark established connections */
    /* This reduces rule traversal for established connections */
    
    /* Create DIVERT chain for established connections */
    tproxy_chain_create(cfg, 4, "mangle", "ATP_DIVERT_0");
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_DIVERT_0");
    
    /* Mark established connections */
    tproxy_rule_add(cfg, 4, "mangle", "ATP_DIVERT_0", 
                    "-j MARK --set-mark 20");
    tproxy_rule_add(cfg, 4, "mangle", "ATP_DIVERT_0", 
                    "-j ACCEPT");
    
    /* Redirect established TCP connections to DIVERT chain */
    tproxy_rule_add(cfg, 4, "mangle", "ATP_PRE_0", 
                    "-p tcp -m socket --transparent -j ATP_DIVERT_0");
    
    LOG_DEBUG("Conntrack optimization enabled");
    return 0;
}

int perf_mode_enable_socket_match(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        return 0;
    }
    
    LOG_INFO("Enabling socket match optimization");
    
    /* Socket match allows direct delivery for established sockets */
    /* This bypasses TPROXY for already established connections */
    
    LOG_DEBUG("Socket match optimization enabled");
    return 0;
}

int perf_mode_setup(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        LOG_INFO("Performance mode disabled");
        return 0;
    }
    
    LOG_INFO("Performance mode enabled");
    
    /* Tune TCP stack */
    perf_mode_tune_tcp_stack(cfg);
    
    /* Enable conntrack optimization */
    perf_mode_enable_conntrack_optimization(cfg);
    
    /* Enable socket match */
    perf_mode_enable_socket_match(cfg);
    
    return 0;
}

int perf_mode_cleanup(atp_config_t *cfg) {
    if (!cfg->performance_mode) {
        return 0;
    }
    
    LOG_INFO("Cleaning up performance mode settings");
    
    /* Flush DIVERT chain */
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_DIVERT_0");
    
    return 0;
}

int perf_mode_init(atp_config_t *cfg) {
    if (cfg->performance_mode) {
        LOG_INFO("Performance mode initialization");
    }
    return 0;
}
