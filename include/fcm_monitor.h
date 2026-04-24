#ifndef ATP_FCM_MONITOR_H
#define ATP_FCM_MONITOR_H

#include "atp.h"
#include <stdint.h>
#include <time.h>

/* Callback when FCM connection is detected */
typedef void (*fcm_callback_t)(const char *remote_ip, uint16_t remote_port, void *userdata);

/* Start FCM monitor thread */
int fcm_monitor_start(fcm_callback_t callback, void *userdata);

/* Stop FCM monitor thread */
void fcm_monitor_stop(void);

/* Check if monitor is running */
int fcm_monitor_is_running(void);

/* Get last detection time (0 if never) */
time_t fcm_monitor_get_last_detection(void);

/* Force refresh of FCM IP cache (for testing) */
void fcm_monitor_refresh_cache(void);

int fcm_monitor_init(atp_config_t *cfg);
void fcm_monitor_poll(void);
void fcm_monitor_cleanup(void);
#endif

int fcm_monitor_get_fd(void);
void fcm_monitor_handle(void);
