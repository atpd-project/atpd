#ifndef ATPD_CONTEXT_H
#define ATPD_CONTEXT_H

#include <stdint.h>
#include <time.h>

typedef enum {
    VPN_STATE_IDLE = 0,
    VPN_STATE_PREDICTING,
    VPN_STATE_READY,
    VPN_STATE_TEARDOWN
} vpn_state_t;

typedef enum {
    ATPD_RUNTIME_STATE_UNINITIALIZED = 0,
    ATPD_RUNTIME_STATE_INITIALIZING,
    ATPD_RUNTIME_STATE_RUNNING,
    ATPD_RUNTIME_STATE_RELOADING,
    ATPD_RUNTIME_STATE_STOPPING,
    ATPD_RUNTIME_STATE_STOPPED,
    ATPD_RUNTIME_STATE_FAILED
} atpd_runtime_state_t;

typedef struct atpd_context atpd_context_t;

typedef struct {
    vpn_state_t state;
    uint32_t if_id;
    char iface[32];
    struct timespec changed_at;
    uint64_t transitions;
} atpd_vpn_snapshot_t;

typedef void (*atpd_vpn_mode_callback_t)(vpn_state_t state,
                                         const char *iface,
                                         void *userdata);

int atpd_context_init(void);

/* VPN observation and owner callbacks. */
int atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface);
void atpd_vpn_get_snapshot(atpd_vpn_snapshot_t *out);
const char *vpn_state_string(vpn_state_t state);
void atpd_set_vpn_mode_callback(atpd_vpn_mode_callback_t callback, void *userdata);
void atpd_set_vpn_teardown_callback(void (*callback)(void));

/* Daemon lifecycle state. */
int atpd_runtime_state_transition(atpd_runtime_state_t new_state);
const char *atpd_runtime_state_string(atpd_runtime_state_t state);
int atpd_runtime_is_running(void);
int atpd_runtime_can_reload(void);
uint64_t atpd_runtime_get_uptime(void);

#endif /* ATPD_CONTEXT_H */
