#ifndef ATPD_CONTEXT_H
#define ATPD_CONTEXT_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <stdbool.h>

typedef enum {
    VPN_STATE_IDLE = 0,
    VPN_STATE_PREDICTING,
    VPN_STATE_READY,
    VPN_STATE_TEARDOWN
} vpn_state_t;

typedef enum {
    EBPF_STATE_UNINITIALIZED = 0,
    EBPF_STATE_LOADING,
    EBPF_STATE_READY,
    EBPF_STATE_FAILED,
    EBPF_STATE_DISABLED
} ebpf_state_t;

typedef enum {
    ATPD_RUNTIME_STATE_UNINITIALIZED = 0,
    ATPD_RUNTIME_STATE_INITIALIZING,
    ATPD_RUNTIME_STATE_RUNNING,
    ATPD_RUNTIME_STATE_RELOADING,
    ATPD_RUNTIME_STATE_STOPPING,
    ATPD_RUNTIME_STATE_STOPPED,
    ATPD_RUNTIME_STATE_FAILED
} atpd_runtime_state_t;

struct atpd_session;
struct atpd_session_list;

typedef struct {
    /* === VPN State === */
    vpn_state_t vpn_state;
    uint32_t xfrm_if_id;
    char vpn_iface[32];
    struct timespec vpn_state_since;
    int xfrm_fd;
    struct atpd_session_list *sessions;
    void (*vpn_teardown_cb)(void);
    uint64_t vpn_transitions;
    uint64_t splice_bytes_total;

    /* === eBPF State === */
    ebpf_state_t ebpf_state;
    bool ebpf_enabled;
    bool ebpf_probed;
    char ebpf_pin_dir[256];

    /* === Runtime State === */
    atpd_runtime_state_t runtime_state;
    uint64_t start_time;
    uint64_t uptime_seconds;
    uint32_t reload_count;
    uint32_t error_count;
    uint64_t last_activity_time;

    /* === Component Status === */
    struct {
        bool netlink_ready;
        bool ebpf_ready;
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

/* eBPF State */
void atpd_ebpf_state_transition(ebpf_state_t new_state);
const char* ebpf_state_string(ebpf_state_t state);

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

/* Error */

/* Session */
struct atpd_session_list {
    struct atpd_session *session;
    struct atpd_session_list *next;
};

void atpd_session_register_to_ctx(struct atpd_session *s);
void atpd_session_unregister_from_ctx(struct atpd_session *s);
void atpd_vpn_killswitch(void);

#endif /* ATPD_CONTEXT_H */
