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
#define DEFAULT_TABLE_ID    150
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

#define EBPF_PROBE_TIMEOUT_SEC    10
#define EBPF_PROBE_RETRY_COUNT    3
#define EBPF_PROBE_RETRY_DELAY_SEC 2

#define NETLINK_RECV_TIMEOUT_MS   3000
#define NETLINK_DEBOUNCE_MS       500

/* Service configuration defaults */
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

typedef struct {
    /* Core */
    int foreground;
    int verbose;
    char data_dir[PATH_MAX];
    char core_user[64];
    char core_group[64];
    char pid_file[PATH_MAX];

    /* Network */
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

    /* Interface */
    char mobile_iface[IFNAMSIZ];
    char wifi_iface[IFNAMSIZ];
    char hotspot_iface[IFNAMSIZ];
    char usb_iface[IFNAMSIZ];
    char hotspot_subnet_ipv4[64];
    char hotspot_subnet_ipv6[64];
    int proxy_mobile;
    int proxy_wifi;
    int proxy_hotspot;
    int proxy_usb;

    /* Filters */
    int app_proxy_enable;
    char app_proxy_mode[32];
    char other_proxy[4096];
    char other_bypass[4096];
    char bypass_ipv4_list[4096];
    char bypass_ipv6_list[4096];
    char proxy_ipv4_list[4096];
    char proxy_ipv6_list[4096];
    int mac_filter_enable;
    char mac_proxy_mode[32];
    int bypass_cn_ip;
    char cn_ip_url[256];
    char cn_ip_file[64];
    char cn_ipv6_url[256];
    char cn_ipv6_file[64];
    int cnip_mode;
    char cnip_force_proxy_apps[4096];
    char bypass_apps_list[4096];
    char proxy_apps_list[4096];

    /* Performance */
    int performance_mode;
    int dry_run;

    /* API */
    int api_port;
    char api_host[64];

    /* eBPF */
    int ebpf_enabled;
    int ebpf_ready;
    char ebpf_config_path[PATH_MAX];
    char ebpf_pin_dir[PATH_MAX];
    char ebpf_state_dir[PATH_MAX];
    int ebpf_load_retry;
    int ebpf_load_delay;

    /* UI */
    int ui_emoji_enabled;
    int no_color;

    /* Service configuration */
    int service_start_timeout_sec;
    int service_stop_timeout_sec;
    int service_grace_period_sec;
    int service_max_failures;
    int service_circuit_threshold;
    int service_circuit_cooldown_sec;
    int service_health_check_interval_ms;
    char service_args[512];
    char service_env[512];

} atp_config_t;

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
