#ifndef ATP_CONFIG_H
#define ATP_CONFIG_H

#include <stdbool.h>
#include <limits.h>

/* Core */
typedef struct {
    bool ui_emoji_enabled;
    bool log_timestamp;
    char data_dir[PATH_MAX];
    char run_dir[PATH_MAX];
    char core_user[64];
    char core_group[64];
    char pid_file[PATH_MAX];
    char log_file[PATH_MAX];
} core_config_t;

/* Interface & VPN Sensing */
typedef struct {
    bool vpn_auto_mode;
    char vpn_target_mode[64];
    char vpn_fallback_mode[64];
} interface_config_t;

/* Service Supervisor */
typedef struct {
    int restart_delay_sec;
    int start_timeout_sec;
    int stop_timeout_sec;
    int grace_period_sec;
    int max_failures;
    int circuit_threshold;
    int circuit_cooldown_sec;
    int health_check_interval_ms;
    char args[256];
    char env[256];
} service_config_t;

/* API Client */
typedef struct {
    int port;
    char host[64];
    char secret[128];
} api_config_t;

/* Complete config */
typedef struct {
    core_config_t core;
    interface_config_t interface;
    service_config_t service;
    api_config_t api;
} atp_config_t;

#endif
