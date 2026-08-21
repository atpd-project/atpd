#ifndef ATP_H
#define ATP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <pthread.h>
#include "atp_error.h"
#include "atp_config.h"

#define ATP_NAME            "atpd"
#define ATP_BUILD_TIME      __TIME__

#define ATP_VERSION_MAJOR   1
#define ATP_VERSION_MINOR   0
#define ATP_VERSION_PATCH   0

#ifndef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 3
#endif

#define ATP_DEFAULT_DIR     "/data/adb/atp"
#define ATP_CONF_FILE       "atp.conf"
#define ATP_PID_FILE        "run/atpd.pid"
#define ATP_LOG_FILE        "run/atp.log"
#define ATP_RUNTIME_CONF    "run/runtime_atp.conf"

#define PROXY_BIN_NAME      "sing-box"
#define PROXY_BIN_PATH      ATP_DEFAULT_DIR "/bin/sing-box"

#define DEFAULT_TCP_PORT    1536
#define DEFAULT_UDP_PORT    1536
#define DEFAULT_REDIRECT_TCP_PORT  7891
#define DEFAULT_MARK        20
#define DEFAULT_MARK6       25
#define DEFAULT_TABLE_ID    2025
#define DEFAULT_DNS_PORT    1053
#define DEFAULT_RESTART_DELAY 2
#define DEFAULT_API_PORT    9090
#define DEFAULT_API_HOST    "127.0.0.1"

#define CMD_TIMEOUT_SEC     5
#define API_RETRY_COUNT     3
#define API_RETRY_DELAY_MS  500
#define API_MIN_INTERVAL_MS 10000

#define QUEUE_SIZE          64
#define MAX_ARGS            32
#define MAX_CMD_LEN         512
#define MAX_OUTPUT_LEN      4096
#define MAX_IFACE_NAME      32
#define MAX_IP_STR          64

#define SERVICE_STOP_RETRY_COUNT  50
#define SERVICE_STOP_INTERVAL_MS  100
#define SERVICE_STOP_TIMEOUT_MS   5000

#define NETLINK_RECV_TIMEOUT_MS   3000
#define NETLINK_DEBOUNCE_MS       6000

#define SERVICE_DEFAULT_START_TIMEOUT_SEC 30
#define SERVICE_DEFAULT_STOP_TIMEOUT_SEC 10
#define SERVICE_DEFAULT_GRACE_PERIOD_SEC 3
#define SERVICE_DEFAULT_MAX_FAILURES 5
#define SERVICE_DEFAULT_CIRCUIT_THRESHOLD 5
#define SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC 60
#define SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS 5000

typedef enum {
    MODE_AUTO = 0,
    MODE_TPROXY = 1,
    MODE_REDIRECT = 2,
    MODE_ENHANCE = 3
} proxy_mode_t;

typedef enum {
    DNS_HIJACK_OFF = 0,
    DNS_HIJACK_TPROXY = 1,
    DNS_HIJACK_REDIRECT = 2
} dns_hijack_mode_t;

typedef enum {
    ROOT_UNKNOWN = 0,
    ROOT_KSU = 1,
    ROOT_MAGISK = 2
} root_method_t;

/* ========== Compatibility Macros ========== */
#define cfg_foreground          cfg->core.foreground
#define cfg_verbose             cfg->core.verbose
#define cfg_no_color            cfg->core.no_color
#define cfg_ui_emoji_enabled    cfg->core.ui_emoji_enabled
#define cfg_performance_mode    cfg->core.performance_mode
#define cfg_dry_run             cfg->core.dry_run
#define cfg_skip_check_feature  cfg->core.skip_check_feature
#define cfg_force_mark_bypass   cfg->core.force_mark_bypass
#define cfg_log_timestamp       cfg->core.log_timestamp
#define cfg_proxy_tcp           cfg->core.proxy_tcp
#define cfg_proxy_udp           cfg->core.proxy_udp
#define cfg_block_quic          cfg->core.block_quic
#define cfg_restart_delay       cfg->core.restart_delay
#define cfg_data_dir            cfg->core.data_dir
#define cfg_core_user           cfg->core.core_user
#define cfg_core_group          cfg->core.core_group
#define cfg_pid_file            cfg->core.pid_file
#define cfg_routing_mark        cfg->core.routing_mark

#define cfg_use_tproxy          cfg->network.use_tproxy
#define cfg_proxy_mode          cfg->network.proxy_mode
#define cfg_tcp_port            cfg->network.tcp_port
#define cfg_udp_port            cfg->network.udp_port
#define cfg_redirect_tcp_port   cfg->network.redirect_tcp_port
#define cfg_mark_value          cfg->network.mark_value
#define cfg_mark_value6         cfg->network.mark_value6
#define cfg_table_id            cfg->network.table_id
#define cfg_proxy_ipv6          cfg->network.proxy_ipv6
#define cfg_dns_hijack          cfg->network.dns_hijack
#define cfg_dns_port            cfg->network.dns_port

#define cfg_mobile_iface        cfg->interface.mobile_iface
#define cfg_wifi_iface          cfg->interface.wifi_iface
#define cfg_hotspot_iface       cfg->interface.hotspot_iface
#define cfg_usb_iface           cfg->interface.usb_iface
#define cfg_hotspot_subnet_ipv4 cfg->interface.hotspot_subnet_ipv4
#define cfg_hotspot_subnet_ipv6 cfg->interface.hotspot_subnet_ipv6
#define cfg_current_vpn_iface   cfg->interface.current_vpn_iface
#define cfg_other_proxy         cfg->interface.other_proxy
#define cfg_other_bypass        cfg->interface.other_bypass
#define cfg_proxy_mobile        cfg->interface.proxy_mobile
#define cfg_proxy_wifi          cfg->interface.proxy_wifi
#define cfg_proxy_hotspot       cfg->interface.proxy_hotspot
#define cfg_proxy_usb           cfg->interface.proxy_usb

#define cfg_app_proxy_enable    cfg->filter.app_proxy_enable
#define cfg_mac_filter_enable   cfg->filter.mac_filter_enable
#define cfg_bypass_cn_ip        cfg->filter.bypass_cn_ip
#define cfg_app_proxy_mode      cfg->filter.app_proxy_mode
#define cfg_mac_proxy_mode      cfg->filter.mac_proxy_mode
#define cfg_user_clash_mode     cfg->filter.user_clash_mode
#define cfg_clash_secret        cfg->filter.clash_secret
#define cfg_proxy_apps_list     cfg->filter.proxy_apps_list
#define cfg_bypass_apps_list    cfg->filter.bypass_apps_list
#define cfg_proxy_macs_list     cfg->filter.proxy_macs_list
#define cfg_bypass_macs_list    cfg->filter.bypass_macs_list
#define cfg_cnip_force_proxy_apps cfg->filter.cnip_force_proxy_apps
#define cfg_cn_ip_url           cfg->filter.cn_ip_url
#define cfg_cn_ip_file          cfg->filter.cn_ip_file
#define cfg_cn_ipv6_url         cfg->filter.cn_ipv6_url
#define cfg_cn_ipv6_file        cfg->filter.cn_ipv6_file

#define cfg_bypass_ipv4_list    cfg->iplist.bypass_ipv4_list
#define cfg_bypass_ipv6_list    cfg->iplist.bypass_ipv6_list
#define cfg_proxy_ipv4_list     cfg->iplist.proxy_ipv4_list
#define cfg_proxy_ipv6_list     cfg->iplist.proxy_ipv6_list

#define cfg_service_start_timeout_sec      cfg->service.start_timeout_sec
#define cfg_service_stop_timeout_sec       cfg->service.stop_timeout_sec
#define cfg_service_grace_period_sec       cfg->service.grace_period_sec
#define cfg_service_max_failures           cfg->service.max_failures
#define cfg_service_circuit_threshold      cfg->service.circuit_threshold
#define cfg_service_circuit_cooldown_sec   cfg->service.circuit_cooldown_sec
#define cfg_service_health_check_interval_ms cfg->service.health_check_interval_ms
#define cfg_service_args                   cfg->service.args
#define cfg_service_env                    cfg->service.env

#define cfg_api_port          cfg->api.port
#define cfg_api_host          cfg->api.host

#define cfg_config_mutex      cfg->mutex

int atp_init(void);
int atp_cleanup(void);
int atp_create_pidfile(void);
void atp_remove_pidfile(void);
void atp_daemonize(void);
int atp_signal_setup(void);
int atp_check_running(void);
int atp_check_root(void);
void atp_show_status(void);

#endif
