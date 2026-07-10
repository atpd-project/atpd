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

struct atpd_session;
struct atpd_session_list;

typedef struct {
    vpn_state_t vpn_state;
    uint32_t xfrm_if_id;
    char vpn_iface[32];
    struct timespec vpn_state_since;

    int xfrm_fd;

    struct atpd_session_list *sessions;

    void (*vpn_teardown_cb)(void);

    uint64_t vpn_transitions;
    uint64_t splice_bytes_total;

    ebpf_state_t ebpf_state;
    bool ebpf_enabled;
    bool ebpf_probed;
    char ebpf_pin_dir[256];

} atpd_context_t;

extern atpd_context_t g_atpd_ctx;

void atpd_context_init(void);
void atpd_vpn_state_transition(vpn_state_t new_state, uint32_t if_id, const char *iface);
const char* vpn_state_string(vpn_state_t state);

void atpd_ebpf_state_transition(ebpf_state_t new_state);
const char* ebpf_state_string(ebpf_state_t state);

struct atpd_session_list {
    struct atpd_session *session;
    struct atpd_session_list *next;
};

void atpd_session_register_to_ctx(struct atpd_session *s);
void atpd_session_unregister_from_ctx(struct atpd_session *s);
void atpd_vpn_killswitch(void);

#endif
