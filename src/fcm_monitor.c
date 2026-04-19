#include "fcm_monitor.h"
#include "logger.h"
#include "utils.h"
#include "inet_diag.h"
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#define FCM_PORT 5228
#define FCM_CACHE_TTL 300  /* 5 minutes */
#define FCM_POLL_INTERVAL_MS 1000  /* 1 second */
#define MAX_FCM_IPS 64
#define MAX_TRACKED_CONNS 512
#define TRACKED_TTL 60  /* 60 seconds */

/* Google FCM domains */
static const char *fcm_domains[] = {
    "mtalk.google.com",
    "alt1-mtalk.google.com",
    "alt2-mtalk.google.com",
    "alt3-mtalk.google.com",
    "alt4-mtalk.google.com",
    "alt5-mtalk.google.com",
    "alt6-mtalk.google.com",
    "alt7-mtalk.google.com",
    "alt8-mtalk.google.com",
    NULL
};

/* Cached FCM IP addresses */
typedef struct {
    struct in_addr addr;
    time_t added_time;
} fcm_ip_cache_t;

static fcm_ip_cache_t g_fcm_ips[MAX_FCM_IPS];
static int g_fcm_ip_count = 0;
static time_t g_fcm_cache_time = 0;
static pthread_mutex_t g_fcm_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Tracked connections (avoid duplicate events) */
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    time_t timestamp;
} tracked_conn_t;

static tracked_conn_t g_tracked_conns[MAX_TRACKED_CONNS];
static int g_tracked_count = 0;
static pthread_mutex_t g_tracked_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Monitor thread */
static pthread_t g_monitor_thread;
static int g_monitor_running = 0;
static fcm_callback_t g_callback = NULL;
static void *g_userdata = NULL;

/* Last detection time for status display */
static time_t g_last_detection_time = 0;
static pthread_mutex_t g_last_detection_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Resolve domain to IP address */
static int resolve_domain(const char *domain, struct in_addr *ips, int max_ips) {
    struct addrinfo hints, *res, *p;
    int count = 0;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    if (getaddrinfo(domain, NULL, &hints, &res) != 0) {
        LOG_DEBUG("Failed to resolve %s", domain);
        return 0;
    }
    
    for (p = res; p != NULL && count < max_ips; p = p->ai_next) {
        struct sockaddr_in *addr = (struct sockaddr_in*)p->ai_addr;
        ips[count].s_addr = addr->sin_addr.s_addr;
        count++;
    }
    
    freeaddrinfo(res);
    return count;
}

/* Refresh FCM IP cache */
static void refresh_fcm_ips(void) {
    time_t now = time(NULL);
    
    pthread_mutex_lock(&g_fcm_mutex);
    
    /* Check if cache is still valid */
    if (g_fcm_cache_time > 0 && (now - g_fcm_cache_time) < FCM_CACHE_TTL) {
        pthread_mutex_unlock(&g_fcm_mutex);
        return;
    }
    
    /* Clear old cache */
    g_fcm_ip_count = 0;
    memset(g_fcm_ips, 0, sizeof(g_fcm_ips));
    
    /* Resolve all FCM domains */
    for (int i = 0; fcm_domains[i] != NULL; i++) {
        struct in_addr ips[MAX_FCM_IPS];
        int count = resolve_domain(fcm_domains[i], ips, MAX_FCM_IPS - g_fcm_ip_count);
        
        for (int j = 0; j < count && g_fcm_ip_count < MAX_FCM_IPS; j++) {
            g_fcm_ips[g_fcm_ip_count].addr = ips[j];
            g_fcm_ips[g_fcm_ip_count].added_time = now;
            g_fcm_ip_count++;
        }
    }
    
    g_fcm_cache_time = now;
    
    LOG_DEBUG("FCM IP cache refreshed: %d IPs", g_fcm_ip_count);
    
    pthread_mutex_unlock(&g_fcm_mutex);
}

/* Check if IP is a known FCM server */
static int is_fcm_ip(uint32_t ip) {
    int found = 0;
    
    pthread_mutex_lock(&g_fcm_mutex);
    
    for (int i = 0; i < g_fcm_ip_count; i++) {
        if (g_fcm_ips[i].addr.s_addr == ip) {
            found = 1;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_fcm_mutex);
    return found;
}

/* Check if connection already triggered an event */
static int is_connection_tracked(uint32_t src_ip, uint16_t src_port,
                                  uint32_t dst_ip, uint16_t dst_port) {
    time_t now = time(NULL);
    int found = 0;
    
    pthread_mutex_lock(&g_tracked_mutex);
    
    for (int i = 0; i < g_tracked_count; i++) {
        if (g_tracked_conns[i].src_ip == src_ip &&
            g_tracked_conns[i].src_port == src_port &&
            g_tracked_conns[i].dst_ip == dst_ip &&
            g_tracked_conns[i].dst_port == dst_port &&
            (now - g_tracked_conns[i].timestamp) < TRACKED_TTL) {
            found = 1;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_tracked_mutex);
    return found;
}

/* Add connection to tracking list */
static void add_tracked_connection(uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port) {
    time_t now = time(NULL);
    
    pthread_mutex_lock(&g_tracked_mutex);
    
    /* Remove expired entries */
    int write_idx = 0;
    for (int i = 0; i < g_tracked_count; i++) {
        if ((now - g_tracked_conns[i].timestamp) < TRACKED_TTL) {
            if (write_idx != i) {
                memcpy(&g_tracked_conns[write_idx], &g_tracked_conns[i],
                       sizeof(tracked_conn_t));
            }
            write_idx++;
        }
    }
    g_tracked_count = write_idx;
    
    /* Add new entry */
    if (g_tracked_count < MAX_TRACKED_CONNS) {
        g_tracked_conns[g_tracked_count].src_ip = src_ip;
        g_tracked_conns[g_tracked_count].src_port = src_port;
        g_tracked_conns[g_tracked_count].dst_ip = dst_ip;
        g_tracked_conns[g_tracked_count].dst_port = dst_port;
        g_tracked_conns[g_tracked_count].timestamp = now;
        g_tracked_count++;
    }
    
    pthread_mutex_unlock(&g_tracked_mutex);
}

/* Update last detection time */
static void update_last_detection(void) {
    pthread_mutex_lock(&g_last_detection_mutex);
    g_last_detection_time = time(NULL);
    pthread_mutex_unlock(&g_last_detection_mutex);
}

/* Monitor thread main loop */
static void* fcm_monitor_loop(void *arg) {
    (void)arg;
    
    LOG_INFO("FCM monitor thread started (poll interval: %dms)", FCM_POLL_INTERVAL_MS);
    
    while (g_monitor_running) {
        connection_info_t *conns = NULL;
        int count = 0;
        
        /* Refresh FCM IP cache periodically */
        refresh_fcm_ips();
        
        /* Get all established TCP connections */
        if (inet_diag_get_connections(&conns, &count, IPPROTO_TCP, 0) == 0) {
            for (int i = 0; i < count; i++) {
                /* Check if destination port is 5228 and IP is FCM server */
                if (conns[i].dst_port == FCM_PORT && 
                    conns[i].family == AF_INET &&
                    is_fcm_ip(conns[i].dst.v4.ip)) {
                    
                    /* Check if already tracked */
                    if (!is_connection_tracked(conns[i].src.v4.ip, conns[i].src_port,
                                                conns[i].dst.v4.ip, conns[i].dst_port)) {
                        LOG_INFO("FCM connection detected: %s:%d -> %s:%d",
                                 conns[i].src_ip_str, conns[i].src_port,
                                 conns[i].dst_ip_str, conns[i].dst_port);
                        
                        /* Add to tracking */
                        add_tracked_connection(conns[i].src.v4.ip, conns[i].src_port,
                                               conns[i].dst.v4.ip, conns[i].dst_port);
                        
                        /* Update last detection time */
                        update_last_detection();
                        
                        /* Trigger callback */
                        if (g_callback) {
                            g_callback(conns[i].dst_ip_str, conns[i].dst_port, g_userdata);
                        }
                    }
                }
            }
            inet_diag_free_connections(conns);
        }
        
        /* Sleep for poll interval */
        for (int i = 0; i < FCM_POLL_INTERVAL_MS / 100 && g_monitor_running; i++) {
            usleep(100000);  /* 100ms */
        }
    }
    
    LOG_INFO("FCM monitor thread stopped");
    return NULL;
}

/* Start FCM monitor */
int fcm_monitor_start(fcm_callback_t callback, void *userdata) {
    if (g_monitor_running) {
        LOG_WARN("FCM monitor already running");
        return 0;
    }
    
    if (!callback) {
        LOG_ERROR("FCM monitor requires a callback function");
        return -1;
    }
    
    g_callback = callback;
    g_userdata = userdata;
    g_monitor_running = 1;
    g_last_detection_time = 0;
    
    /* Initial cache refresh */
    refresh_fcm_ips();
    
    if (pthread_create(&g_monitor_thread, NULL, fcm_monitor_loop, NULL) != 0) {
        LOG_ERROR("Failed to create FCM monitor thread");
        g_monitor_running = 0;
        return -1;
    }
    
    LOG_INFO("FCM monitor started");
    return 0;
}

/* Stop FCM monitor */
void fcm_monitor_stop(void) {
    if (!g_monitor_running) {
        return;
    }
    
    g_monitor_running = 0;
    pthread_join(g_monitor_thread, NULL);
    LOG_INFO("FCM monitor stopped");
}

/* Check if monitor is running */
int fcm_monitor_is_running(void) {
    return g_monitor_running;
}

/* Get last detection time (0 if never) */
time_t fcm_monitor_get_last_detection(void) {
    time_t last;
    pthread_mutex_lock(&g_last_detection_mutex);
    last = g_last_detection_time;
    pthread_mutex_unlock(&g_last_detection_mutex);
    return last;
}

/* Force refresh of IP cache (for testing) */
void fcm_monitor_refresh_cache(void) {
    pthread_mutex_lock(&g_fcm_mutex);
    g_fcm_cache_time = 0;
    pthread_mutex_unlock(&g_fcm_mutex);
    refresh_fcm_ips();
}
int fcm_monitor_init(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}

void fcm_monitor_poll(void) {
    /* Polling handled by background thread */
}

void fcm_monitor_cleanup(void) {
    fcm_monitor_stop();
}
