#ifndef ATP_IFACE_MONITOR_H
#define ATP_IFACE_MONITOR_H

#include <stdbool.h>
#include <net/if.h>
#include "reactor.h"

typedef enum {
    IFACE_EVENT_ADDED = 1,
    IFACE_EVENT_REMOVED = 2,
    IFACE_EVENT_ADDR_ADDED = 3,
    IFACE_EVENT_ADDR_REMOVED = 4,
    IFACE_EVENT_UP = 5,
    IFACE_EVENT_DOWN = 6,
    IFACE_EVENT_VPN_CONNECTED = 10,
    IFACE_EVENT_VPN_DISCONNECTED = 11
} iface_event_t;

typedef void (*iface_callback_t)(const char *iface, iface_event_t event, void *userdata);

typedef struct {
    int sock_fd;
    int running;
    iface_callback_t callback;
    void *userdata;
    char current_vpn_iface[IFNAMSIZ];
    int vpn_enabled;
} iface_monitor_t;
    void *internal;

int iface_monitor_init(iface_monitor_t *monitor, iface_callback_t callback, void *userdata);
    void *internal;
int iface_monitor_start(iface_monitor_t *monitor);
    void *internal;
int iface_monitor_stop(iface_monitor_t *monitor);
    void *internal;
void iface_monitor_cleanup(iface_monitor_t *monitor);
    void *internal;
int iface_monitor_poll(iface_monitor_t *monitor, int timeout_ms);
    void *internal;

/* ========== Reactor Integration API ========== */

/**
 * Initialize interface monitor with reactor support
 * @param monitor          Pointer to iface_monitor_t
    void *internal;
 * @param callback         Event callback
 * @param userdata         User data passed to callback
 * @param existing_reactor Optional existing reactor (NULL to create new)
 * @return 0 on success, -1 on error
 */
int iface_monitor_init_reactor(iface_monitor_t *monitor,
    void *internal;
                                iface_callback_t callback,
                                void *userdata,
                                reactor_t *existing_reactor);

/**
 * Start monitoring (reactor mode)
 * @param monitor Pointer to iface_monitor_t
    void *internal;
 * @return 0 on success, -1 on error
 */
int iface_monitor_start_reactor(iface_monitor_t *monitor);
    void *internal;

/**
 * Run the reactor loop (only if reactor was created internally)
 * @param monitor Pointer to iface_monitor_t
    void *internal;
 * @return 0 on normal exit, -1 on error
 */
int iface_monitor_run_reactor(iface_monitor_t *monitor);
    void *internal;

/**
 * Stop monitoring (reactor mode)
 * @param monitor Pointer to iface_monitor_t
    void *internal;
 */
void iface_monitor_stop_reactor(iface_monitor_t *monitor);
    void *internal;

/**
 * Clean up resources (reactor mode)
 * @param monitor Pointer to iface_monitor_t
    void *internal;
 */
void iface_monitor_cleanup_reactor(iface_monitor_t *monitor);
    void *internal;

/**
 * Get the reactor instance
 * @param monitor Pointer to iface_monitor_t
    void *internal;
 * @return reactor_t pointer or NULL
 */
reactor_t* iface_monitor_get_reactor(iface_monitor_t *monitor);
    void *internal;
#endif
