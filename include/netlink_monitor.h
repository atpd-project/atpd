#ifndef ATP_NETLINK_MONITOR_H
#define ATP_NETLINK_MONITOR_H

#include "atp.h"

/* Netlink monitor event types */
typedef enum {
    NL_EVENT_LINK_UP = 1,
    NL_EVENT_LINK_DOWN = 2,
    NL_EVENT_LINK_ADDED = 3,
    NL_EVENT_LINK_REMOVED = 4,
    NL_EVENT_ADDR_ADDED = 5,
    NL_EVENT_ADDR_REMOVED = 6,
    NL_EVENT_ROUTE_ADDED = 7,
    NL_EVENT_ROUTE_REMOVED = 8,
    NL_EVENT_VPN_CONNECTED = 10,
    NL_EVENT_VPN_DISCONNECTED = 11
} nl_event_type_t;

/* Event callback function type */
typedef void (*nl_event_callback_t)(nl_event_type_t event, const char *iface, void *userdata);

/* Monitor configuration */
typedef struct {
    nl_event_callback_t callback;
    void *userdata;
    int monitor_links;
    int monitor_addrs;
    int monitor_routes;
    int monitor_vpn_only;  /* Only report VPN-related events */
} nl_monitor_config_t;

/* Start/stop the netlink monitor thread */
int netlink_monitor_start(nl_monitor_config_t *config);
void netlink_monitor_stop(void);
int netlink_monitor_is_running(void);

/* Default callback that integrates with existing routing module */
void netlink_default_callback(nl_event_type_t event, const char *iface, void *userdata);

#endif
