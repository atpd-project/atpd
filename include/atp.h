/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Core header file with data structures and constants
 */

#ifndef ATP_ATP_H
#define ATP_ATP_H

#ifndef ATP_NAME
#define ATP_NAME "ATP"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_ether.h>

/* Version information */
#ifndef ATP_VERSION
#define ATP_VERSION "1.0.0"
#endif

#ifndef ATP_VERSION_STRING
#define ATP_VERSION_STRING "1.0.0"
#endif

/* Default paths */
#ifndef ATP_DEFAULT_DIR
#define ATP_DEFAULT_DIR "/data/adb/atp"
#endif

#ifndef ATP_CONF_FILE
#define ATP_CONF_FILE "atp.conf"
#endif

#ifndef ATP_PID_FILE
#define ATP_PID_FILE "run/atpd.pid"
#endif

#ifndef ATP_LOG_FILE
#define ATP_LOG_FILE "run/atp.log"
#endif

#ifndef ATP_COMMAND_SOCKET
#define ATP_COMMAND_SOCKET "run/atpd.sock"
#endif

#ifndef ATP_RUNTIME_CONF
#define ATP_RUNTIME_CONF "run/runtime_atp.conf"
#endif

/* Default port values */
#ifndef DEFAULT_TCP_PORT
#define DEFAULT_TCP_PORT 1536
#endif

#ifndef DEFAULT_UDP_PORT
#define DEFAULT_UDP_PORT 1536
#endif

#ifndef DEFAULT_REDIRECT_TCP_PORT
#define DEFAULT_REDIRECT_TCP_PORT 7891
#endif

#ifndef DEFAULT_DNS_PORT
#define DEFAULT_DNS_PORT 1053
#endif

/* Default routing values */
#ifndef DEFAULT_MARK
#define DEFAULT_MARK 20
#endif

#ifndef DEFAULT_MARK6
#define DEFAULT_MARK6 21
#endif

#ifndef DEFAULT_TABLE_ID
#define DEFAULT_TABLE_ID 150
#endif

/* Default API values */
#ifndef DEFAULT_API_PORT
#define DEFAULT_API_PORT 9090
#endif

#ifndef DEFAULT_API_HOST
#define DEFAULT_API_HOST "127.0.0.1"
#endif

/* Default restart delay (seconds) */
#ifndef DEFAULT_RESTART_DELAY
#define DEFAULT_RESTART_DELAY 5
#endif

/* Proxy modes */
#define MODE_AUTO       0
#define MODE_TPROXY     1
#define MODE_REDIRECT   2
#define MODE_ENHANCE    3

/* DNS hijack modes */
#define DNS_HIJACK_OFF       0
#define DNS_HIJACK_TPROXY    1
#define DNS_HIJACK_REDIRECT  2

/* Maximum lengths */
#define MAX_CONFIG_LINE      1024
#define MAX_IFACE_NAME       IFNAMSIZ
#define MAX_IP_LIST_LEN      4096
#define MAX_APP_LIST_LEN     2048
#define MAX_MAC_LIST_LEN     2048
#define MAX_URL_LEN          512
#define MAX_SECRET_LEN       128

/* Chain names for iptables */
#define CHAIN_PRE_0          "ATP_PRE_0"
#define CHAIN_PRE_1          "ATP_PRE_1"
#define CHAIN_OUT_0          "ATP_OUT_0"
#define CHAIN_OUT_1          "ATP_OUT_1"
#define CHAIN_DNS_0          "ATP_DNS_0"
#define CHAIN_DNS_1          "ATP_DNS_1"
#define CHAIN_XFRM_BYPASS    "XFRM_BYPASS"

/* IPSet names */
#define IPSET_CN_IP          "cnip"
#define IPSET_CN_IP6         "cnip6"
#define IPSET_PROXY_IPV4     "proxy_ipv4"
#define IPSET_PROXY_IPV6     "proxy_ipv6"
#define IPSET_BYPASS_IPV4    "bypass_ipv4"
#define IPSET_BYPASS_IPV6    "bypass_ipv6"

/* Binary paths */
#define SINGBOX_BIN          "/data/adb/atp/bin/sing-box"
#define PROXY_BIN_NAME       "sing-box"
#define PROXY_BIN_PATH       SINGBOX_BIN

/* Command timeout */
#define CMD_TIMEOUT_SEC      10

/* API retry and rate limit configuration */
#define API_RETRY_COUNT 3
#define API_RETRY_DELAY_MS 500
#define API_MIN_INTERVAL_MS 10000

/* Configuration structure */
typedef struct {
    /* Mutex for thread-safe access */
    pthread_mutex_t config_mutex;
    
    /* Directory paths */
    char data_dir[PATH_MAX];
    
    /* Runtime flags */
    int dry_run;
    int verbose;
    int foreground;
    
    /* Proxy ports */
    int tcp_port;
    int udp_port;
    int redirect_tcp_port;
    
    /* Core settings */
    int proxy_mode;
    int performance_mode;
    int proxy_tcp;
    int proxy_udp;
    int proxy_ipv6;
    int skip_check_feature;
    
    /* DNS settings */
    int dns_hijack;
    int dns_port;
    
    /* Routing marks */
    int mark_value;
    int mark_value6;
    int table_id;
    char routing_mark[64];
    int force_mark_bypass;
    
    /* Interface names */
    char mobile_iface[MAX_IFACE_NAME];
    char wifi_iface[MAX_IFACE_NAME];
    char hotspot_iface[MAX_IFACE_NAME];
    char usb_iface[MAX_IFACE_NAME];
    char other_bypass[MAX_IFACE_NAME];
    char other_proxy[MAX_IFACE_NAME];
    
    /* Interface proxy flags */
    int proxy_mobile;
    int proxy_wifi;
    int proxy_hotspot;
    int proxy_usb;
    
    /* Hotspot subnets */
    char hotspot_subnet_ipv4[64];
    char hotspot_subnet_ipv6[64];
    
    /* Custom IP lists */
    char proxy_ipv4_list[MAX_IP_LIST_LEN];
    char proxy_ipv6_list[MAX_IP_LIST_LEN];
    char bypass_ipv4_list[MAX_IP_LIST_LEN];
    char bypass_ipv6_list[MAX_IP_LIST_LEN];
    
    /* CN IP bypass */
    int bypass_cn_ip;
    char cn_ip_file[64];
    char cn_ipv6_file[64];
    char cn_ip_url[MAX_URL_LEN];
    char cn_ipv6_url[MAX_URL_LEN];
    
    /* Per-app proxy */
    int app_proxy_enable;
    char proxy_apps_list[MAX_APP_LIST_LEN];
    char bypass_apps_list[MAX_APP_LIST_LEN];
    char app_proxy_mode[16];
    
    /* MAC filter */
    int mac_filter_enable;
    char proxy_macs_list[MAX_MAC_LIST_LEN];
    char bypass_macs_list[MAX_MAC_LIST_LEN];
    char mac_proxy_mode[16];
    
    /* Features */
    int block_quic;
    int log_timestamp;
    
    /* Clash/API */
    char user_clash_mode[32];
    int restart_delay;
    char clash_secret[MAX_SECRET_LEN];
    char api_host[64];
    int api_port;
    
    /* User/group for core process */
    char core_user[32];
    char core_group[32];
    
    /* Runtime state (not from config file) */
    int use_tproxy;
    char current_vpn_iface[MAX_IFACE_NAME];
    
    /* UI settings */
    int ui_emoji_enabled;   /* 1 = use emoji, 0 = use ASCII labels */
} atp_config_t;

#endif /* ATP_ATP_H */
