#ifndef ATPD_CONTEXT_H
#define ATPD_CONTEXT_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>

/* ========== VPN State Machine ========== */

typedef enum {
    VPN_STATE_IDLE = 0,         /* No VPN activity */
    VPN_STATE_PREDICTING,       /* XFRM SA detected, interface not yet up */
    VPN_STATE_READY,            /* VPN interface fully operational */
    VPN_STATE_TEARDOWN          /* XFRM SA deleted, cleaning up */
} vpn_state_t;

/* ========== Splice Session Forward Declaration ========== */

struct atpd_session;
struct atpd_session_list;

/* ========== Global ATPd Context ========== */

typedef struct {
    /* VPN tracking */
    vpn_state_t vpn_state;
    uint32_t xfrm_if_id;
    char vpn_iface[32];
    struct timespec vpn_state_since;  /* Timestamp of last state change */
    
    /* XFRM file descriptor */
    int xfrm_fd;
    
    /* Active splice sessions (linked list) */
    struct atpd_session_list *sessions;
    
    /* Kill-switch: cleanup callback when VPN dies */
    void (*vpn_teardown_cb)(void);
    
    /* Statistics */
    uint64_t vpn_transitions;
    uint64_t splice_bytes_total;
} atpd_context_t;

/* ========== Global Instance ========== */

extern atpd_context_t g_atpd_ctx;

/* ========== API ========== */

void atpd_context_init(void);
void atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface);
const char* vpn_state_string(vpn_state_t state);

/* ========== Session List for Kill-Switch ========== */

struct atpd_session_list {
    struct atpd_session *session;
    struct atpd_session_list *next;
};

/* Register/unregister sessions for VPN teardown cleanup */
void atpd_session_register_to_ctx(struct atpd_session *s);
void atpd_session_unregister_from_ctx(struct atpd_session *s);

/* Kill-switch: close all sessions */
void atpd_vpn_killswitch(void);

#endif /* ATPD_CONTEXT_H */
