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

#define ATP_VERSION_MAJOR   2
#define ATP_VERSION_MINOR   0
#define ATP_VERSION_PATCH   0

#ifndef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 3
#endif

#ifndef ATP_DEFAULT_DIR
#define ATP_DEFAULT_DIR     "/data/adb/atp"
#endif

#ifndef ATP_CONF_FILE
#define ATP_CONF_FILE       "atp.conf"
#endif

#ifndef ATP_RUN_DIR
#define ATP_RUN_DIR         "run"
#endif

#ifndef ATP_PID_FILE
#define ATP_PID_FILE        ATP_RUN_DIR "/atpd.pid"
#endif

#ifndef ATP_LOG_FILE
#define ATP_LOG_FILE        ATP_RUN_DIR "/atp.log"
#endif

#ifndef ATP_COMMAND_SOCKET
#define ATP_COMMAND_SOCKET  ATP_RUN_DIR "/atpd.sock"
#endif

#ifndef ATP_RUNTIME_CONF
#define ATP_RUNTIME_CONF    ATP_RUN_DIR "/runtime_atp.conf"
#endif

#define PROXY_BIN_NAME      "sing-box"
#define PROXY_PID_FILE      ATP_RUN_DIR "/sing-box.pid"
#define PROXY_LOG_FILE      ATP_RUN_DIR "/sing-box.log"
#define TRAFFIC_STATE_FILE  ATP_RUN_DIR "/traffic.state"
#define PROXY_BIN_PATH      ATP_DEFAULT_DIR "/bin/sing-box"

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

#define SERVICE_DEFAULT_START_TIMEOUT_SEC 30
#define SERVICE_DEFAULT_STOP_TIMEOUT_SEC 10
#define SERVICE_DEFAULT_GRACE_PERIOD_SEC 3
#define SERVICE_DEFAULT_MAX_FAILURES 5
#define SERVICE_DEFAULT_CIRCUIT_THRESHOLD 5
#define SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC 60
#define SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS 5000

typedef enum {
    MODE_AUTO = 0,
    MODE_EBPF = 4
} proxy_mode_t;

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
#define cfg_log_timestamp       cfg->core.log_timestamp
#define cfg_restart_delay       cfg->core.restart_delay
#define cfg_data_dir            cfg->core.data_dir
#define cfg_core_user           cfg->core.core_user
#define cfg_core_group          cfg->core.core_group
#define cfg_pid_file            cfg->core.pid_file

#define cfg_proxy_mode          cfg->network.proxy_mode
#define cfg_proxy_ipv6          cfg->network.proxy_ipv6
#define cfg_dns_hijack          cfg->network.dns_hijack

#define cfg_current_vpn_iface   cfg->interface.current_vpn_iface

#define cfg_ebpf_enabled        cfg->ebpf.enabled
#define cfg_ebpf_ready          cfg->ebpf.ready

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
#define cfg_clash_secret      cfg->api.secret

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
