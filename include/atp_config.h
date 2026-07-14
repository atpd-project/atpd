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
    bool performance_mode;
    bool dry_run;
    bool skip_check_feature;
    bool force_mark_bypass;
    bool log_timestamp;
    bool proxy_tcp;
    bool proxy_udp;
    bool block_quic;
    int restart_delay;
    char data_dir[PATH_MAX];
    char core_user[64];
    char core_group[64];
    char pid_file[PATH_MAX];
    char routing_mark[32];
} core_config_t;

/* Network */
typedef struct {
    int use_tproxy;
    int proxy_mode;
    int tcp_port;
    int udp_port;
    int redirect_tcp_port;
    int mark_value;
    int mark_value6;
    int table_id;
    int proxy_ipv6;
    int dns_hijack;
    int dns_port;
} network_config_t;

/* Interface */
typedef struct {
    char mobile_iface[IFNAMSIZ];
    char wifi_iface[IFNAMSIZ];
    char hotspot_iface[IFNAMSIZ];
    char usb_iface[IFNAMSIZ];
    char hotspot_subnet_ipv4[64];
    char hotspot_subnet_ipv6[64];
    char current_vpn_iface[IFNAMSIZ];
    char other_proxy[4096];
    char other_bypass[4096];
    int proxy_mobile;
    int proxy_wifi;
    int proxy_hotspot;
    int proxy_usb;
} interface_config_t;

/* Filter */
typedef struct {
    bool app_proxy_enable;
    bool mac_filter_enable;
    bool bypass_cn_ip;
    int cnip_mode;
    char app_proxy_mode[32];
    char mac_proxy_mode[32];
    char user_clash_mode[32];
    char clash_secret[128];
    char proxy_apps_list[4096];
    char bypass_apps_list[4096];
    char proxy_macs_list[4096];
    char bypass_macs_list[4096];
    char cnip_force_proxy_apps[4096];
    char cn_ip_url[256];
    char cn_ip_file[64];
    char cn_ipv6_url[256];
    char cn_ipv6_file[64];
} filter_config_t;

/* IP Lists */
typedef struct {
    char bypass_ipv4_list[4096];
    char bypass_ipv6_list[4096];
    char proxy_ipv4_list[4096];
    char proxy_ipv6_list[4096];
} iplist_config_t;

/* eBPF */
typedef struct {
    bool enabled;
    bool ready;
    int load_retry;
    int load_delay;
    char bin_path[PATH_MAX];
    char config_path[PATH_MAX];
    char pin_dir[PATH_MAX];
    char state_dir[PATH_MAX];
} ebpf_config_t;

/* Service */
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

/* API */
typedef struct {
    int port;
    char host[64];
} api_config_t;

/* Complete config */
typedef struct {
    core_config_t core;
    network_config_t network;
    interface_config_t interface;
    filter_config_t filter;
    iplist_config_t iplist;
    ebpf_config_t ebpf;
    service_config_t service;
    api_config_t api;
    pthread_mutex_t mutex;
} atp_config_t;

#endif
