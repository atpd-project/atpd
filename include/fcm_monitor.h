#ifndef ATP_FCM_MONITOR_H
#define ATP_FCM_MONITOR_H

#include "atp_config.h"
#include <stdint.h>
#include <time.h>
#include <stdatomic.h>

/* FCM default port */
#define FCM_DEFAULT_PORT 5228

/* Callback type for FCM connection events */
typedef void (*fcm_callback_t)(const char *dst_ip, uint16_t dst_port, void *userdata);

/* Initialize FCM monitor */
int fcm_monitor_init(atp_config_t *cfg);

/* Start FCM monitor thread */
int fcm_monitor_start(fcm_callback_t callback, void *userdata);

/* Stop FCM monitor thread */
void fcm_monitor_stop(void);

/* Check if monitor is running */
int fcm_monitor_is_running(void);

/* Poll FCM connections (for non-threaded mode) */
void fcm_monitor_poll(void);

/* Get last detection time */
time_t fcm_monitor_get_last_detection(void);

/* Force refresh IP cache */
void fcm_monitor_refresh_cache(void);

/* Get monitor FD (for reactor integration) */
int fcm_monitor_get_fd(void);

/* Handle monitor events (for reactor integration) */
void fcm_monitor_handle(void);

/* Cleanup FCM monitor resources */
void fcm_monitor_cleanup(void);

#endif
