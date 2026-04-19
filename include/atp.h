#ifndef ATP_H
#define ATP_H

/* Project name */
#ifndef ATP_NAME
#define ATP_NAME "ATP"
#endif

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

#define ATP_VERSION         "1.0.0"
#define ATP_NAME            "ATP (Advanced Transparent Proxy)"
#define ATP_BUILD_DATE      __DATE__
#define ATP_BUILD_TIME      __TIME__

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
    char data_dir[PATH_MAX];
    char conf_file[PATH_MAX];
    int dry_run;
    int verbose;
    int foreground;
    
    int tcp_port;
    int udp_port;
    int redirect_tcp_port;      /* REDIRECT port for ENHANCE mode */
    proxy_mode_t proxy_mode;
    int performance_mode;
    int proxy_tcp;
    int proxy_udp;
    int proxy_ipv6;
    
    dns_hijack_mode_t dns_hijack;
    int dns_port;
    
    int mark_value;
    int mark_value6;
    int table_id;
    char core_user[64];
    char core_group[64];
    char routing_mark[32];
    int force_mark_bypass;
    
    char mobile_iface[64];
    char wifi_iface[64];
    char hotspot_iface[64];
    char usb_iface[64];
    char other_bypass[512];
    char other_proxy[512];
    int proxy_mobile;
    int proxy_wifi;
    int proxy_hotspot;
    int proxy_usb;
    
    char hotspot_subnet_ipv4[32];
    char hotspot_subnet_ipv6[64];
    
    char proxy_ipv4_list[4096];
    char proxy_ipv6_list[4096];
    char bypass_ipv4_list[4096];
    char bypass_ipv6_list[4096];
    
    int bypass_cn_ip;
    char cn_ip_file[256];
    char cn_ipv6_file[256];
    char cn_ip_url[512];
    char cn_ipv6_url[512];
    
    int app_proxy_enable;
    char proxy_apps_list[4096];
    char bypass_apps_list[4096];
    char app_proxy_mode[16];
    
    int mac_filter_enable;
    char proxy_macs_list[4096];
    char bypass_macs_list[4096];
    char mac_proxy_mode[16];
    
    int block_quic;
    int log_timestamp;
    int skip_check_feature;
    char user_clash_mode[32];
    int restart_delay;
    char clash_secret[128];
    
    /* API configuration */
    int api_port;
    char api_host[64];
    
    int use_tproxy;
    char current_vpn_iface[32];
    
    /* Thread safety */
    pthread_mutex_t config_mutex;
} atp_config_t;

extern atp_config_t g_config;

int atp_init(void);
void atp_cleanup(void);
int atp_create_pidfile(void);
void atp_remove_pidfile(void);
void atp_daemonize(void);
int atp_signal_setup(void);
int atp_check_running(void);
int atp_check_root(void);
void atp_show_status(void);

#endif
