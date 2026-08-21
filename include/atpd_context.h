#ifndef ATPD_CONTEXT_H
#define ATPD_CONTEXT_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <limits.h>

typedef enum {
    VPN_STATE_IDLE = 0,
    VPN_STATE_PREDICTING,
    VPN_STATE_READY,
    VPN_STATE_TEARDOWN
} vpn_state_t;

typedef enum {
    DIRECT_WIFI_DISABLED = 0,
    DIRECT_WIFI_DISCONNECTED,
    DIRECT_WIFI_ACTIVE
} direct_wifi_state_t;

typedef struct {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_bytes_per_sec;
    uint64_t tx_bytes_per_sec;
    uint64_t sampled_at;
    bool rate_ready;
} vpn_traffic_t;

typedef enum {
    ATPD_RUNTIME_STATE_UNINITIALIZED = 0,
    ATPD_RUNTIME_STATE_INITIALIZING,
    ATPD_RUNTIME_STATE_RUNNING,
    ATPD_RUNTIME_STATE_RELOADING,
    ATPD_RUNTIME_STATE_STOPPING,
    ATPD_RUNTIME_STATE_STOPPED,
    ATPD_RUNTIME_STATE_FAILED
} atpd_runtime_state_t;

typedef struct {
    /* === VPN State === */
    atomic_int vpn_state;           /* Changed to atomic */
    uint32_t xfrm_if_id;
    char vpn_iface[32];
    char other_vpn_iface[32];
    vpn_traffic_t google_vpn_traffic;
    vpn_traffic_t other_vpn_traffic;
    struct timespec vpn_state_since;
    int xfrm_fd;
    uint64_t vpn_transitions;

    /* === VPN Policy Snapshot === */
    uint32_t vpn_route_table;
    bool vpn_ipv4_default;
    bool vpn_ipv6_default;
    char hotspot_ifaces[256];
    unsigned hotspot_count;
    unsigned hotspot_ipv4_active;
    unsigned hotspot_ipv6_active;
    uint64_t policy_last_reconcile;

    /* === Clash Mode Snapshot === */
    char clash_desired_mode[32];
    char clash_applied_mode[32];
    char clash_last_error[128];
    uint64_t clash_last_sync;
    uint64_t fcm_last_seen;
    bool fcm_monitor_active;

    /* === Direct Wi-Fi State === */
    atomic_int direct_wifi_state;
    char current_wifi_ssid[128];
    char direct_wifi_restore_mode[32];
    struct timespec direct_wifi_state_since;
    uint64_t direct_wifi_transitions;

    /* === Runtime State === */
    atpd_runtime_state_t runtime_state;
    uint64_t start_time;
    uint64_t uptime_seconds;
    uint32_t reload_count;
    uint32_t error_count;
    uint64_t last_activity_time;
    char config_path[PATH_MAX];

    /* === Component Status === */
    struct {
        bool netlink_ready;
        bool service_ready;
        bool api_ready;
        bool reactor_ready;
    } components;

    /* === Statistics === */
    struct {
        uint64_t events_processed;
        uint64_t timers_fired;
        uint64_t signals_received;
        uint64_t errors_total;
        uint64_t bytes_rx;
        uint64_t bytes_tx;
    } stats;

    /* === Last Error === */
    struct {
        uint32_t last_error_code;
        char last_error_msg[128];
        uint64_t last_error_time;
    } last_error;

} atpd_context_t;

extern atpd_context_t g_atpd_ctx;

void atpd_context_init(void);

/* VPN State */
void atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface);
const char* vpn_state_string(vpn_state_t state);
const char* direct_wifi_state_string(direct_wifi_state_t state);
const char* atpd_clash_target_mode(vpn_state_t vpn_state,
                                   direct_wifi_state_t wifi_state,
                                   const char *configured_mode);
void atpd_direct_wifi_state_transition(direct_wifi_state_t new_state,
                                       const char *ssid);

/* Runtime State */
void atpd_runtime_state_transition(atpd_runtime_state_t new_state);
const char* atpd_runtime_state_string(atpd_runtime_state_t state);
int atpd_runtime_is_running(void);
int atpd_runtime_can_reload(void);
void atpd_runtime_update_uptime(void);
uint64_t atpd_runtime_get_uptime(void);

/* Component Status */
void atpd_component_set_ready(const char *name, int ready);
int atpd_component_is_ready(const char *name);

/* Statistics */
void atpd_stats_increment_events(void);
void atpd_stats_increment_timers(void);
void atpd_stats_increment_signals(void);
void atpd_stats_increment_errors(void);
void atpd_stats_add_bytes(uint64_t rx, uint64_t tx);

#endif /* ATPD_CONTEXT_H */
