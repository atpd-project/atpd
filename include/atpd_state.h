#ifndef ATPD_STATE_H
#define ATPD_STATE_H

#include <stdint.h>

typedef enum {
    ATPD_STATE_UNINITIALIZED = 0,
    ATPD_STATE_INITIALIZING,
    ATPD_STATE_RUNNING,
    ATPD_STATE_RELOADING,
    ATPD_STATE_STOPPING,
    ATPD_STATE_STOPPED,
    ATPD_STATE_FAILED
} atpd_state_t;

void atpd_state_machine_init(void *ctx);
int atpd_state_transition(atpd_state_t new_state, void *ctx);
const char *atpd_state_to_string(atpd_state_t state);
int atpd_state_is_running(void);

#endif
