#ifndef ATP_CONFIG_TYPES_H
#define ATP_CONFIG_TYPES_H

#include <limits.h>
#include <net/if.h>
#include <pthread.h>

typedef struct {
    char data_dir[PATH_MAX];
    char core_user[64];
    char core_group[64];
    int dry_run;
} core_config_t;

typedef struct {
    char backend[16];
} network_config_t;

typedef struct {
    char hotspot_iface[IFNAMSIZ];
    int hotspot_iface_explicit;
    char current_vpn_iface[IFNAMSIZ];
} interface_config_t;

typedef struct {
    char user_clash_mode[32];
    char clash_secret[128];
    char direct_wifi_ssid[128];
} filter_config_t;

typedef struct {
    int start_timeout_sec;
    int stop_timeout_sec;
    int grace_period_sec;
    int max_failures;
    int circuit_threshold;
    int circuit_cooldown_sec;
    int health_check_interval_ms;
    char args[512];
    char env[512];
} service_config_t;

typedef struct {
    int port;
    char host[64];
} api_config_t;

typedef struct {
    core_config_t core;
    network_config_t network;
    interface_config_t interface;
    filter_config_t filter;
    service_config_t service;
    api_config_t api;
    pthread_mutex_t mutex;
} atp_config_t;

#endif
