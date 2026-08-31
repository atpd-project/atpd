#include "atpd_context.h"

#include <stdio.h>
#include <time.h>

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_NONE
} log_level_t;

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    atpd_vpn_snapshot_t snapshot;
    struct timespec delay = { .tv_sec = 1, .tv_nsec = 0 };

    CHECK(atpd_context_init() == 0);
    CHECK(atpd_context_init() == -1);
    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_INITIALIZING) == 0);
    CHECK(!atpd_runtime_can_reload());
    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING) == 0);
    nanosleep(&delay, NULL);

    uint64_t before_reload = atpd_runtime_get_uptime();
    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RELOADING) == 0);
    CHECK(!atpd_runtime_can_reload());
    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING) == 0);
    CHECK(atpd_runtime_get_uptime() >= before_reload);

    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING) == 0);
    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPED) == 0);
    CHECK(atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING) == -1);

    CHECK(atpd_vpn_state_transition(VPN_STATE_READY, 1, "ipsec0") == 0);
    atpd_vpn_get_snapshot(&snapshot);
    CHECK(snapshot.state == VPN_STATE_READY);
    CHECK(snapshot.if_id == 1);
    CHECK(snapshot.transitions == 1);
    CHECK(atpd_vpn_state_transition(VPN_STATE_READY, 2, "ipsec0") == 0);
    atpd_vpn_get_snapshot(&snapshot);
    CHECK(snapshot.if_id == 2);
    CHECK(snapshot.transitions == 2);

    puts("ATPD context ownership tests passed");
    return 0;
}
