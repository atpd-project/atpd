#ifndef ATP_CONFIG_H
#define ATP_CONFIG_H

#include <stdbool.h>
#include <limits.h>
#include <net/if.h>
#include <pthread.h>

/* Core */
typedef struct {
    bool foreground;
    bool verbose;
    bool no_color;
    bool ui_emoji_enabled;
    bool dry_run;
    bool log_timestamp;
    int restart_delay;
    char data_dir[PATH_MAX];
    char run_dir[PATH_MAX];
    char core_user[64];
    char core_group[64];
    char pid_file[PATH_MAX];
    char log_file[PATH_MAX];
} core_config_t;

/* Interface & VPN Sensing */
typedef struct {
    char current_vpn_iface[IFNAMSIZ];
    bool vpn_auto_mode;
    char vpn_target_mode[64];
    char vpn_fallback_mode[64];
} interface_config_t;

/* eBPF Prober Config */
typedef struct {
    bool enabled;
    bool ready;
} ebpf_config_t;

/* Service Supervisor */
typedef struct {
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
    ebpf_config_t ebpf;
    service_config_t service;
    api_config_t api;
    pthread_mutex_t mutex;
} atp_config_t;

#endif
