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
#include "atp_result.h"
#include "atp_config.h"

#define ATP_NAME            "atpd"
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
#define PROXY_LOG_FILE      "sing-box.log"
#define TRAFFIC_STATE_FILE  ATP_RUN_DIR "/traffic.state"
#define PROXY_BIN_PATH      ATP_DEFAULT_DIR "/bin/sing-box"

#define DEFAULT_API_PORT    9080
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
#define NETLINK_DEBOUNCE_MS       500

#define SERVICE_DEFAULT_START_TIMEOUT_SEC 30
#define SERVICE_DEFAULT_STOP_TIMEOUT_SEC 10
#define SERVICE_DEFAULT_GRACE_PERIOD_SEC 3
#define SERVICE_DEFAULT_MAX_FAILURES 5
#define SERVICE_DEFAULT_CIRCUIT_THRESHOLD 5
#define SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC 60
#define SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS 5000

typedef enum {
    ROOT_UNKNOWN = 0,
    ROOT_KSU = 1,
    ROOT_MAGISK = 2
} root_method_t;

#endif
