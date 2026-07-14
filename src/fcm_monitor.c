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
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <assert.h>

#define FCM_PORT 5228
#define FCM_CACHE_TTL 300
#define FCM_POLL_INTERVAL_MS 1000
#define MAX_FCM_IPS 256
#define MAX_TRACKED_CONNS 1024
#define TRACKED_TTL 600
#define DNS_REFRESH_INTERVAL 300
#define TRACKED_CLEANUP_INTERVAL 60

/* Inode compatibility */
#ifndef HAVE_INET_DIAG_INODE
#define CONN_INODE(c) (0ULL)
#else
#define CONN_INODE(c) ((c)->inode)
#endif

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

typedef struct {
    int family;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } addr;
} fcm_ip_t;

typedef struct {
    fcm_ip_t ip;
} fcm_ip_cache_entry_t;

typedef struct {
    fcm_ip_cache_entry_t entries[MAX_FCM_IPS];
    int count;
    uint64_t cache_time;
    uint64_t last_refresh_attempt;
    uint64_t resolved_domain_count;
    uint64_t failed_domain_count;
    uint64_t discarded_count;
} fcm_cache_t;

typedef struct {
    sa_family_t family;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } src;
    union {
        struct in_addr v4;
        struct in6_addr v6;
    } dst;
    uint16_t src_port;
    uint16_t dst_port;
    uint64_t timestamp;
    uint64_t inode;
    uint8_t has_inode;
} tracked_conn_t;

typedef struct {
    atomic_uint_fast64_t dns_refresh_success;
    atomic_uint_fast64_t dns_refresh_failed;
    atomic_uint_fast64_t tracked_table_full;
    atomic_uint_fast64_t cache_full;
    atomic_uint_fast64_t resolved_domain_total;
    atomic_uint_fast64_t failed_domain_total;
    atomic_uint_fast64_t dns_refresh_duration_ms;
    atomic_uint_fast64_t dns_refresh_start_ms;
    atomic_uint_fast64_t dns_refresh_end_ms;
    atomic_uint_fast64_t last_detection_wallclock;
    atomic_uint_fast64_t last_detection_monotonic;
} fcm_monitor_internal_stats_t;

typedef struct {
    atomic_int running;
    atomic_int initialized;
    atomic_flag dns_refreshing;
    pthread_t thread;
    pthread_mutex_t callback_mutex;
    pthread_mutex_t cache_mutex;
    pthread_mutex_t tracked_mutex;
    fcm_callback_t callback;
    void *userdata;
    fcm_cache_t cache_a;
    fcm_cache_t cache_b;
    fcm_cache_t *active_cache;
    fcm_cache_t *staging_cache;
    tracked_conn_t tracked[MAX_TRACKED_CONNS];
    int tracked_count;
    uint64_t last_tracked_cleanup;
    fcm_monitor_internal_stats_t stats;
} fcm_monitor_ctx_t;

static fcm_monitor_ctx_t g_ctx;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static atomic_int g_destroyed;

static void init_destroyed(void) {
    atomic_init(&g_destroyed, 0);
}

static uint64_t monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec;
    }
    return (uint64_t)time(NULL);
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    }
    return 0;
}

static int ipv4_exists(fcm_ip_t *ips, int count, struct in_addr *addr) {
    for (int i = 0; i < count; i++) {
        if (ips[i].family == AF_INET &&
            ips[i].addr.v4.s_addr == addr->s_addr) {
            return 1;
        }
    }
    return 0;
}

static int ipv6_exists(fcm_ip_t *ips, int count, struct in6_addr *addr) {
    for (int i = 0; i < count; i++) {
        if (ips[i].family == AF_INET6 &&
            memcmp(&ips[i].addr.v6, addr, sizeof(struct in6_addr)) == 0) {
            return 1;
        }
    }
    return 0;
}

static int cache_ip_exists(fcm_ip_cache_entry_t *entries, int count, fcm_ip_t *ip) {
    for (int i = 0; i < count; i++) {
        if (entries[i].ip.family != ip->family) continue;

        if (ip->family == AF_INET) {
            if (entries[i].ip.addr.v4.s_addr == ip->addr.v4.s_addr) {
                return 1;
            }
        } else if (ip->family == AF_INET6) {
            if (memcmp(&entries[i].ip.addr.v6, &ip->addr.v6,
                       sizeof(struct in6_addr)) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int resolve_domain(const char *domain, fcm_ip_t *ips, int max_ips) {
    struct addrinfo hints, *res, *p;
    int count = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(domain, NULL, &hints, &res) != 0) {
        return 0;
    }

    for (p = res; p != NULL && count < max_ips; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in*)p->ai_addr;
            if (!ipv4_exists(ips, count, &addr->sin_addr)) {
                ips[count].family = AF_INET;
                ips[count].addr.v4 = addr->sin_addr;
                count++;
            }
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *addr = (struct sockaddr_in6*)p->ai_addr;
            if (!ipv6_exists(ips, count, &addr->sin6_addr)) {
                ips[count].family = AF_INET6;
                ips[count].addr.v6 = addr->sin6_addr;
                count++;
            }
        }
    }

    freeaddrinfo(res);
    return count;
}

static void verify_monitor_state(void) {
#ifdef FCM_DEBUG
    assert(g_ctx.active_cache != NULL);
    assert(g_ctx.staging_cache != NULL);
    assert(g_ctx.active_cache != g_ctx.staging_cache);
    assert(g_ctx.tracked_count >= 0 && g_ctx.tracked_count <= MAX_TRACKED_CONNS);
#else
    if (g_ctx.active_cache == NULL || g_ctx.staging_cache == NULL ||
        g_ctx.active_cache == g_ctx.staging_cache) {
        LOG_ERROR("FCM: invalid cache state");
    }
#endif
}

static int inode_still_exists(uint64_t inode, const uint64_t *active_inodes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (active_inodes[i] == inode) {
            return 1;
        }
    }
    return 0;
}

static void add_inode_to_set(uint64_t *set, size_t *count, uint64_t inode) {
    if (*count >= MAX_TRACKED_CONNS) return;
    for (size_t i = 0; i < *count; i++) {
        if (set[i] == inode) return;
    }
    set[(*count)++] = inode;
}

static void refresh_fcm_ips(void) {
    uint64_t now = monotonic_seconds();
    fcm_cache_t new_cache;
    int total_ips = 0;
    int ipv4_count = 0;
    int ipv6_count = 0;
    uint64_t resolved_count = 0;
    uint64_t failed_count = 0;
    uint64_t discarded_count = 0;
    uint64_t start_ms = monotonic_ms();
    uint64_t duration_ms;

    if (atomic_flag_test_and_set(&g_ctx.dns_refreshing)) {
        return;
    }

    pthread_mutex_lock(&g_ctx.cache_mutex);
    uint64_t cache_time = g_ctx.active_cache->cache_time;
    uint64_t last_refresh = g_ctx.active_cache->last_refresh_attempt;
    pthread_mutex_unlock(&g_ctx.cache_mutex);

    if (now - cache_time < FCM_CACHE_TTL) {
        atomic_flag_clear(&g_ctx.dns_refreshing);
        return;
    }

    if (now - last_refresh < 10) {
        atomic_flag_clear(&g_ctx.dns_refreshing);
        return;
    }

    atomic_store(&g_ctx.stats.dns_refresh_start_ms, start_ms);

    memset(&new_cache, 0, sizeof(new_cache));
    new_cache.cache_time = now;
    new_cache.last_refresh_attempt = now;

    for (int i = 0; fcm_domains[i] != NULL && total_ips < MAX_FCM_IPS; i++) {
        fcm_ip_t ips[MAX_FCM_IPS];
        int count = resolve_domain(fcm_domains[i], ips, MAX_FCM_IPS - total_ips);

        if (count > 0) {
            resolved_count++;
            int inserted = 0;

            for (int j = 0; j < count && total_ips < MAX_FCM_IPS; j++) {
                if (cache_ip_exists(new_cache.entries, total_ips, &ips[j])) {
                    continue;
                }
                if (ips[j].family == AF_INET) {
                    ipv4_count++;
                } else if (ips[j].family == AF_INET6) {
                    ipv6_count++;
                }
                new_cache.entries[total_ips].ip = ips[j];
                total_ips++;
                inserted++;
            }

            discarded_count += (count - inserted);
        } else {
            failed_count++;
        }
    }

    if (discarded_count > 0) {
        LOG_WARN("FCM: DNS cache full, capacity=%d, discarded=%lu",
                 MAX_FCM_IPS, (unsigned long)discarded_count);
        atomic_fetch_add(&g_ctx.stats.cache_full, discarded_count);
    }

    new_cache.resolved_domain_count = resolved_count;
    new_cache.failed_domain_count = failed_count;
    new_cache.discarded_count = discarded_count;
    new_cache.count = total_ips;

    duration_ms = monotonic_ms() - start_ms;

    pthread_mutex_lock(&g_ctx.cache_mutex);

    fcm_cache_t *old_active = g_ctx.active_cache;

    if (total_ips > 0) {
        *g_ctx.staging_cache = new_cache;
        g_ctx.active_cache = g_ctx.staging_cache;
        g_ctx.staging_cache = old_active;

        atomic_fetch_add(&g_ctx.stats.dns_refresh_success, 1);
        atomic_fetch_add(&g_ctx.stats.resolved_domain_total, resolved_count);
        atomic_fetch_add(&g_ctx.stats.failed_domain_total, failed_count);
        atomic_store(&g_ctx.stats.dns_refresh_duration_ms, duration_ms);
        atomic_store(&g_ctx.stats.dns_refresh_end_ms, monotonic_ms());

        LOG_INFO("FCM: DNS refresh success: %d IPs (IPv4=%d, IPv6=%d, domains: OK=%lu, FAIL=%lu, duration=%lums)",
                 total_ips, ipv4_count, ipv6_count,
                 (unsigned long)resolved_count, (unsigned long)failed_count,
                 (unsigned long)duration_ms);
    } else {
        g_ctx.staging_cache->last_refresh_attempt = now;
        atomic_fetch_add(&g_ctx.stats.dns_refresh_failed, 1);
        atomic_fetch_add(&g_ctx.stats.failed_domain_total, failed_count);
        atomic_store(&g_ctx.stats.dns_refresh_duration_ms, duration_ms);
        atomic_store(&g_ctx.stats.dns_refresh_end_ms, monotonic_ms());

        LOG_WARN("FCM: DNS refresh failed, keeping previous cache (%d IPs, domains: OK=%lu, FAIL=%lu)",
                 g_ctx.active_cache->count,
                 (unsigned long)resolved_count, (unsigned long)failed_count);
    }

    assert(g_ctx.active_cache != g_ctx.staging_cache);
    verify_monitor_state();

    pthread_mutex_unlock(&g_ctx.cache_mutex);
    atomic_flag_clear(&g_ctx.dns_refreshing);
}

static int is_fcm_ip(const void *ip, int family) {
    int found = 0;

    pthread_mutex_lock(&g_ctx.cache_mutex);

    fcm_cache_t *cache = g_ctx.active_cache;

    for (int i = 0; i < cache->count; i++) {
        if (family == AF_INET && cache->entries[i].ip.family == AF_INET) {
            const struct in_addr *addr = (const struct in_addr*)ip;
            if (cache->entries[i].ip.addr.v4.s_addr == addr->s_addr) {
                found = 1;
                break;
            }
        } else if (family == AF_INET6 && cache->entries[i].ip.family == AF_INET6) {
            const struct in6_addr *addr = (const struct in6_addr*)ip;
            if (memcmp(&cache->entries[i].ip.addr.v6, addr,
                       sizeof(struct in6_addr)) == 0) {
                found = 1;
                break;
            }
        }
    }

    pthread_mutex_unlock(&g_ctx.cache_mutex);
    return found;
}

static void cleanup_tracked_connections(const uint64_t *active_inodes, size_t active_count) {
    uint64_t now = monotonic_seconds();
    int write_idx = 0;

    pthread_mutex_lock(&g_ctx.tracked_mutex);

    for (int i = 0; i < g_ctx.tracked_count; i++) {
        if (g_ctx.tracked[i].has_inode) {
            if (inode_still_exists(g_ctx.tracked[i].inode, active_inodes, active_count)) {
                if (write_idx != i) {
                    g_ctx.tracked[write_idx] = g_ctx.tracked[i];
                }
                write_idx++;
            }
            continue;
        }

        if (now - g_ctx.tracked[i].timestamp < TRACKED_TTL) {
            if (write_idx != i) {
                g_ctx.tracked[write_idx] = g_ctx.tracked[i];
            }
            write_idx++;
        }
    }

    int removed = g_ctx.tracked_count - write_idx;
    g_ctx.tracked_count = write_idx;
    g_ctx.last_tracked_cleanup = now;

    if (removed > 0) {
        LOG_DEBUG("FCM: cleaned up %d expired/inactive tracked connections", removed);
    }

    pthread_mutex_unlock(&g_ctx.tracked_mutex);
}

static int is_connection_tracked_by_inode(uint64_t inode, sa_family_t family) {
    int found = 0;

    if (inode == 0) return 0;

    pthread_mutex_lock(&g_ctx.tracked_mutex);

    for (int i = 0; i < g_ctx.tracked_count; i++) {
        if (g_ctx.tracked[i].inode == inode &&
            g_ctx.tracked[i].family == family) {
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&g_ctx.tracked_mutex);
    return found;
}

static int is_connection_tracked_v4(uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port) {
    int found = 0;

    pthread_mutex_lock(&g_ctx.tracked_mutex);

    for (int i = 0; i < g_ctx.tracked_count; i++) {
        if (g_ctx.tracked[i].family == AF_INET &&
            g_ctx.tracked[i].src.v4.s_addr == src_ip &&
            g_ctx.tracked[i].src_port == src_port &&
            g_ctx.tracked[i].dst.v4.s_addr == dst_ip &&
            g_ctx.tracked[i].dst_port == dst_port) {
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&g_ctx.tracked_mutex);
    return found;
}

static int is_connection_tracked_v6(const struct in6_addr *src_ip,
                                     uint16_t src_port,
                                     const struct in6_addr *dst_ip,
                                     uint16_t dst_port) {
    int found = 0;

    pthread_mutex_lock(&g_ctx.tracked_mutex);

    for (int i = 0; i < g_ctx.tracked_count; i++) {
        if (g_ctx.tracked[i].family == AF_INET6 &&
            memcmp(&g_ctx.tracked[i].src.v6.s6_addr, src_ip->s6_addr, 16) == 0 &&
            g_ctx.tracked[i].src_port == src_port &&
            memcmp(&g_ctx.tracked[i].dst.v6.s6_addr, dst_ip->s6_addr, 16) == 0 &&
            g_ctx.tracked[i].dst_port == dst_port) {
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&g_ctx.tracked_mutex);
    return found;
}

static void add_tracked_connection_v4(uint32_t src_ip, uint16_t src_port,
                                       uint32_t dst_ip, uint16_t dst_port,
                                       uint64_t inode) {
    uint64_t now = monotonic_seconds();

    pthread_mutex_lock(&g_ctx.tracked_mutex);

    if (now - g_ctx.last_tracked_cleanup > TRACKED_CLEANUP_INTERVAL) {
        pthread_mutex_unlock(&g_ctx.tracked_mutex);
        pthread_mutex_lock(&g_ctx.tracked_mutex);
    }

    if (inode != 0) {
        for (int i = 0; i < g_ctx.tracked_count; i++) {
            if (g_ctx.tracked[i].inode == inode &&
                g_ctx.tracked[i].family == AF_INET) {
                g_ctx.tracked[i].timestamp = now;
                pthread_mutex_unlock(&g_ctx.tracked_mutex);
                return;
            }
        }
    }

    for (int i = 0; i < g_ctx.tracked_count; i++) {
        if (g_ctx.tracked[i].family == AF_INET &&
            g_ctx.tracked[i].src.v4.s_addr == src_ip &&
            g_ctx.tracked[i].src_port == src_port &&
            g_ctx.tracked[i].dst.v4.s_addr == dst_ip &&
            g_ctx.tracked[i].dst_port == dst_port) {
            g_ctx.tracked[i].timestamp = now;
            if (inode != 0 && !g_ctx.tracked[i].has_inode) {
                g_ctx.tracked[i].inode = inode;
                g_ctx.tracked[i].has_inode = 1;
            }
            pthread_mutex_unlock(&g_ctx.tracked_mutex);
            return;
        }
    }

    if (g_ctx.tracked_count < MAX_TRACKED_CONNS) {
        g_ctx.tracked[g_ctx.tracked_count].family = AF_INET;
        g_ctx.tracked[g_ctx.tracked_count].src.v4.s_addr = src_ip;
        g_ctx.tracked[g_ctx.tracked_count].src_port = src_port;
        g_ctx.tracked[g_ctx.tracked_count].dst.v4.s_addr = dst_ip;
        g_ctx.tracked[g_ctx.tracked_count].dst_port = dst_port;
        g_ctx.tracked[g_ctx.tracked_count].timestamp = now;
        g_ctx.tracked[g_ctx.tracked_count].inode = inode;
        g_ctx.tracked[g_ctx.tracked_count].has_inode = (inode != 0);
        g_ctx.tracked_count++;
    } else {
        atomic_fetch_add(&g_ctx.stats.tracked_table_full, 1);
        if (atomic_load(&g_ctx.stats.tracked_table_full) == 1 ||
            now - g_ctx.last_tracked_cleanup > 60) {
            LOG_WARN("FCM: tracked connection table full (%d entries)",
                     MAX_TRACKED_CONNS);
        }

        uint64_t oldest_time = g_ctx.tracked[0].timestamp;
        int oldest_idx = 0;
        for (int i = 1; i < g_ctx.tracked_count; i++) {
            if (g_ctx.tracked[i].timestamp < oldest_time) {
                oldest_time = g_ctx.tracked[i].timestamp;
                oldest_idx = i;
            }
        }
        g_ctx.tracked[oldest_idx].family = AF_INET;
        g_ctx.tracked[oldest_idx].src.v4.s_addr = src_ip;
        g_ctx.tracked[oldest_idx].src_port = src_port;
        g_ctx.tracked[oldest_idx].dst.v4.s_addr = dst_ip;
        g_ctx.tracked[oldest_idx].dst_port = dst_port;
        g_ctx.tracked[oldest_idx].timestamp = now;
        g_ctx.tracked[oldest_idx].inode = inode;
        g_ctx.tracked[oldest_idx].has_inode = (inode != 0);
    }

    pthread_mutex_unlock(&g_ctx.tracked_mutex);
}

static void add_tracked_connection_v6(const struct in6_addr *src_ip,
                                       uint16_t src_port,
                                       const struct in6_addr *dst_ip,
                                       uint16_t dst_port,
                                       uint64_t inode) {
    uint64_t now = monotonic_seconds();

    pthread_mutex_lock(&g_ctx.tracked_mutex);

    if (now - g_ctx.last_tracked_cleanup > TRACKED_CLEANUP_INTERVAL) {
        pthread_mutex_unlock(&g_ctx.tracked_mutex);
        pthread_mutex_lock(&g_ctx.tracked_mutex);
    }

    if (inode != 0) {
        for (int i = 0; i < g_ctx.tracked_count; i++) {
            if (g_ctx.tracked[i].inode == inode &&
                g_ctx.tracked[i].family == AF_INET6) {
                g_ctx.tracked[i].timestamp = now;
                pthread_mutex_unlock(&g_ctx.tracked_mutex);
                return;
            }
        }
    }

    for (int i = 0; i < g_ctx.tracked_count; i++) {
        if (g_ctx.tracked[i].family == AF_INET6 &&
            memcmp(&g_ctx.tracked[i].src.v6.s6_addr, src_ip->s6_addr, 16) == 0 &&
            g_ctx.tracked[i].src_port == src_port &&
            memcmp(&g_ctx.tracked[i].dst.v6.s6_addr, dst_ip->s6_addr, 16) == 0 &&
            g_ctx.tracked[i].dst_port == dst_port) {
            g_ctx.tracked[i].timestamp = now;
            if (inode != 0 && !g_ctx.tracked[i].has_inode) {
                g_ctx.tracked[i].inode = inode;
                g_ctx.tracked[i].has_inode = 1;
            }
            pthread_mutex_unlock(&g_ctx.tracked_mutex);
            return;
        }
    }

    if (g_ctx.tracked_count < MAX_TRACKED_CONNS) {
        g_ctx.tracked[g_ctx.tracked_count].family = AF_INET6;
        memcpy(&g_ctx.tracked[g_ctx.tracked_count].src.v6.s6_addr,
               src_ip->s6_addr, 16);
        g_ctx.tracked[g_ctx.tracked_count].src_port = src_port;
        memcpy(&g_ctx.tracked[g_ctx.tracked_count].dst.v6.s6_addr,
               dst_ip->s6_addr, 16);
        g_ctx.tracked[g_ctx.tracked_count].dst_port = dst_port;
        g_ctx.tracked[g_ctx.tracked_count].timestamp = now;
        g_ctx.tracked[g_ctx.tracked_count].inode = inode;
        g_ctx.tracked[g_ctx.tracked_count].has_inode = (inode != 0);
        g_ctx.tracked_count++;
    } else {
        atomic_fetch_add(&g_ctx.stats.tracked_table_full, 1);
        if (atomic_load(&g_ctx.stats.tracked_table_full) == 1 ||
            now - g_ctx.last_tracked_cleanup > 60) {
            LOG_WARN("FCM: tracked connection table full (%d entries)",
                     MAX_TRACKED_CONNS);
        }

        uint64_t oldest_time = g_ctx.tracked[0].timestamp;
        int oldest_idx = 0;
        for (int i = 1; i < g_ctx.tracked_count; i++) {
            if (g_ctx.tracked[i].timestamp < oldest_time) {
                oldest_time = g_ctx.tracked[i].timestamp;
                oldest_idx = i;
            }
        }
        g_ctx.tracked[oldest_idx].family = AF_INET6;
        memcpy(&g_ctx.tracked[oldest_idx].src.v6.s6_addr,
               src_ip->s6_addr, 16);
        g_ctx.tracked[oldest_idx].src_port = src_port;
        memcpy(&g_ctx.tracked[oldest_idx].dst.v6.s6_addr,
               dst_ip->s6_addr, 16);
        g_ctx.tracked[oldest_idx].dst_port = dst_port;
        g_ctx.tracked[oldest_idx].timestamp = now;
        g_ctx.tracked[oldest_idx].inode = inode;
        g_ctx.tracked[oldest_idx].has_inode = (inode != 0);
    }

    pthread_mutex_unlock(&g_ctx.tracked_mutex);
}

static void* fcm_monitor_loop(void *arg) {
    (void)arg;
    uint64_t last_dns_refresh = 0;
    uint64_t last_tracked_cleanup = 0;

    LOG_INFO("FCM: monitor thread started (poll interval: %dms)", FCM_POLL_INTERVAL_MS);

    while (atomic_load(&g_ctx.running)) {
        uint64_t now = monotonic_seconds();
        uint64_t active_inodes[MAX_TRACKED_CONNS];
        size_t active_count = 0;

        if (now - last_dns_refresh > DNS_REFRESH_INTERVAL) {
            refresh_fcm_ips();
            last_dns_refresh = now;
        }

        connection_info_t *conns = NULL;
        int count = 0;

        if (inet_diag_get_connections(&conns, &count, IPPROTO_TCP, 0) == 0) {
            for (int i = 0; i < count; i++) {
                if (conns[i].dst_port == FCM_PORT) {
                    uint64_t inode = CONN_INODE(&conns[i]);
                    if (inode != 0) {
                        add_inode_to_set(active_inodes, &active_count, inode);
                    }
                }
            }

            for (int i = 0; i < count; i++) {
                if (conns[i].dst_port != FCM_PORT) continue;

                if (conns[i].family == AF_INET) {
                    if (is_fcm_ip(&conns[i].dst.v4.ip, AF_INET)) {
                        uint64_t inode = CONN_INODE(&conns[i]);
                        if (inode != 0) {
                            if (is_connection_tracked_by_inode(inode, AF_INET)) {
                                continue;
                            }
                        } else if (is_connection_tracked_v4(conns[i].src.v4.ip,
                                                              conns[i].src_port,
                                                              conns[i].dst.v4.ip,
                                                              conns[i].dst_port)) {
                            continue;
                        }

                        LOG_INFO("FCM: IPv4 connection detected: %s:%d -> %s:%d (inode=%lu)",
                                 conns[i].src_ip_str, conns[i].src_port,
                                 conns[i].dst_ip_str, conns[i].dst_port,
                                 (unsigned long)inode);

                        add_tracked_connection_v4(conns[i].src.v4.ip,
                                                  conns[i].src_port,
                                                  conns[i].dst.v4.ip,
                                                  conns[i].dst_port,
                                                  inode);

                        atomic_store(&g_ctx.stats.last_detection_monotonic, now);
                        atomic_store(&g_ctx.stats.last_detection_wallclock, time(NULL));

                        fcm_callback_t cb;
                        void *userdata;

                        pthread_mutex_lock(&g_ctx.callback_mutex);
                        cb = g_ctx.callback;
                        userdata = g_ctx.userdata;
                        pthread_mutex_unlock(&g_ctx.callback_mutex);

                        if (cb) {
                            cb(conns[i].dst_ip_str, conns[i].dst_port, userdata);
                        }
                    }
                } else if (conns[i].family == AF_INET6) {
                    if (is_fcm_ip(&conns[i].dst.v6.ip, AF_INET6)) {
                        uint64_t inode = CONN_INODE(&conns[i]);
                        if (inode != 0) {
                            if (is_connection_tracked_by_inode(inode, AF_INET6)) {
                                continue;
                            }
                       } else if (is_connection_tracked_v6(&conns[i].src.v6,conns[i].src.v6.ip,
                                     conns[i].src_port,
                                     &conns[i].dst.v6,conns[i].dst.v6.ip,
                                     conns[i].dst_port)) {

                        LOG_INFO("FCM: IPv6 connection detected: %s:%d -> %s:%d (inode=%lu)",
                                 conns[i].src_ip_str, conns[i].src_port,
                                 conns[i].dst_ip_str, conns[i].dst_port,
                                 (unsigned long)inode);

                        } else if (is_connection_tracked_v6(&conns[i].src.v6.ip,
                                     conns[i].src_port,
                                     &conns[i].dst.v6.ip,
                                     conns[i].dst_port)) {

                        atomic_store(&g_ctx.stats.last_detection_monotonic, now);
                        atomic_store(&g_ctx.stats.last_detection_wallclock, time(NULL));

                        fcm_callback_t cb;
                        void *userdata;

                        pthread_mutex_lock(&g_ctx.callback_mutex);
                        cb = g_ctx.callback;
                        userdata = g_ctx.userdata;
                        pthread_mutex_unlock(&g_ctx.callback_mutex);

                        if (cb) {
                            cb(conns[i].dst_ip_str, conns[i].dst_port, userdata);
                        }
                    }
                }
            }
            inet_diag_free_connections(conns);
        }

        if (now - last_tracked_cleanup > TRACKED_CLEANUP_INTERVAL) {
            cleanup_tracked_connections(active_inodes, active_count);
            last_tracked_cleanup = now;
        }

        for (int i = 0; i < FCM_POLL_INTERVAL_MS / 100 && atomic_load(&g_ctx.running); i++) {
            usleep(100000);
        }
    }

    LOG_INFO("FCM: monitor thread stopped");
    return NULL;
}

/* ========== Initialization ========== */

static void fcm_monitor_do_init(void) {
    atomic_init(&g_ctx.running, 0);
    atomic_init(&g_ctx.initialized, 1);
    atomic_flag_clear(&g_ctx.dns_refreshing);

    pthread_mutex_init(&g_ctx.callback_mutex, NULL);
    pthread_mutex_init(&g_ctx.cache_mutex, NULL);
    pthread_mutex_init(&g_ctx.tracked_mutex, NULL);

    atomic_init(&g_ctx.stats.dns_refresh_success, 0);
    atomic_init(&g_ctx.stats.dns_refresh_failed, 0);
    atomic_init(&g_ctx.stats.tracked_table_full, 0);
    atomic_init(&g_ctx.stats.cache_full, 0);
    atomic_init(&g_ctx.stats.resolved_domain_total, 0);
    atomic_init(&g_ctx.stats.failed_domain_total, 0);
    atomic_init(&g_ctx.stats.dns_refresh_duration_ms, 0);
    atomic_init(&g_ctx.stats.last_detection_monotonic, 0);
    atomic_init(&g_ctx.stats.last_detection_wallclock, 0);
    atomic_init(&g_ctx.stats.dns_refresh_start_ms, 0);
    atomic_init(&g_ctx.stats.dns_refresh_end_ms, 0);

    g_ctx.cache_a.count = 0;
    g_ctx.cache_a.cache_time = 0;
    g_ctx.cache_a.last_refresh_attempt = 0;
    g_ctx.cache_a.resolved_domain_count = 0;
    g_ctx.cache_a.failed_domain_count = 0;
    g_ctx.cache_a.discarded_count = 0;
    g_ctx.cache_b.count = 0;
    g_ctx.cache_b.cache_time = 0;
    g_ctx.cache_b.last_refresh_attempt = 0;
    g_ctx.cache_b.resolved_domain_count = 0;
    g_ctx.cache_b.failed_domain_count = 0;
    g_ctx.cache_b.discarded_count = 0;
    g_ctx.active_cache = &g_ctx.cache_a;
    g_ctx.staging_cache = &g_ctx.cache_b;
    g_ctx.last_tracked_cleanup = 0;
    g_ctx.tracked_count = 0;

    assert(g_ctx.active_cache != g_ctx.staging_cache);
    LOG_DEBUG("FCM: initialized");
}

/* ========== Public API ========== */

int fcm_monitor_init(atp_config_t *cfg) {
    (void)cfg;

    init_destroyed();

    if (atomic_load(&g_destroyed)) {
        LOG_ERROR("FCM: monitor already destroyed, cannot reinit");
        return -1;
    }

    pthread_once(&g_init_once, fcm_monitor_do_init);
    return 0;
}

int fcm_monitor_start(fcm_callback_t callback, void *userdata) {
    if (atomic_load(&g_destroyed)) {
        LOG_ERROR("FCM: monitor already destroyed, cannot start");
        return -1;
    }

    if (!callback) {
        LOG_ERROR("FCM: callback required");
        return -1;
    }

    if (atomic_load(&g_ctx.running)) {
        LOG_WARN("FCM: already running");
        return 0;
    }

    pthread_mutex_lock(&g_ctx.callback_mutex);
    g_ctx.callback = callback;
    g_ctx.userdata = userdata;
    pthread_mutex_unlock(&g_ctx.callback_mutex);

    atomic_store(&g_ctx.stats.last_detection_monotonic, 0);
    atomic_store(&g_ctx.stats.last_detection_wallclock, 0);

    refresh_fcm_ips();

    atomic_store(&g_ctx.running, 1);

    if (pthread_create(&g_ctx.thread, NULL, fcm_monitor_loop, NULL) != 0) {
        LOG_ERROR("FCM: failed to create thread");
        atomic_store(&g_ctx.running, 0);

        pthread_mutex_lock(&g_ctx.callback_mutex);
        g_ctx.callback = NULL;
        g_ctx.userdata = NULL;
        pthread_mutex_unlock(&g_ctx.callback_mutex);

        return -1;
    }

    LOG_INFO("FCM: started");
    return 0;
}

void fcm_monitor_stop(void) {
    if (!atomic_load(&g_ctx.running)) {
        return;
    }

    int self_thread = pthread_equal(pthread_self(), g_ctx.thread);

    if (self_thread) {
        atomic_store(&g_ctx.running, 0);
        LOG_INFO("FCM: stop called from monitor thread, marking stopped");
        return;
    }

    atomic_store(&g_ctx.running, 0);
    pthread_join(g_ctx.thread, NULL);

    LOG_INFO("FCM: stopped");
}

int fcm_monitor_is_running(void) {
    return atomic_load(&g_ctx.running);
}

time_t fcm_monitor_get_last_detection(void) {
    return (time_t)atomic_load(&g_ctx.stats.last_detection_wallclock);
}

int fcm_monitor_get_stats(fcm_monitor_stats_t *stats) {
    if (!stats) return -1;

    stats->dns_refresh_success = atomic_load(&g_ctx.stats.dns_refresh_success);
    stats->dns_refresh_failed = atomic_load(&g_ctx.stats.dns_refresh_failed);
    stats->tracked_table_full = atomic_load(&g_ctx.stats.tracked_table_full);
    stats->cache_full = atomic_load(&g_ctx.stats.cache_full);
    stats->resolved_domain_total = atomic_load(&g_ctx.stats.resolved_domain_total);
    stats->failed_domain_total = atomic_load(&g_ctx.stats.failed_domain_total);
    stats->dns_duration_ms = atomic_load(&g_ctx.stats.dns_refresh_duration_ms);
    stats->last_detection = (time_t)atomic_load(&g_ctx.stats.last_detection_wallclock);

    pthread_mutex_lock(&g_ctx.tracked_mutex);
    stats->tracked_entries = (uint64_t)g_ctx.tracked_count;
    pthread_mutex_unlock(&g_ctx.tracked_mutex);

    pthread_mutex_lock(&g_ctx.cache_mutex);
    stats->cache_entries = (uint64_t)g_ctx.active_cache->count;
    pthread_mutex_unlock(&g_ctx.cache_mutex);

    return 0;
}

void fcm_monitor_refresh_cache(void) {
    if (atomic_load(&g_destroyed)) {
        LOG_WARN("FCM: monitor destroyed, cannot refresh cache");
        return;
    }

    pthread_mutex_lock(&g_ctx.cache_mutex);
    g_ctx.active_cache->cache_time = 0;
    pthread_mutex_unlock(&g_ctx.cache_mutex);
    refresh_fcm_ips();
}

void fcm_monitor_poll(void) {
    /* Polling handled by background thread */
}

int fcm_monitor_get_fd(void) {
    return -1;
}

void fcm_monitor_handle(void) {
    fcm_monitor_poll();
}

void fcm_monitor_cleanup(void) {
    if (atomic_load(&g_destroyed)) {
        return;
    }

    fcm_monitor_stop();

    pthread_mutex_destroy(&g_ctx.callback_mutex);
    pthread_mutex_destroy(&g_ctx.cache_mutex);
    pthread_mutex_destroy(&g_ctx.tracked_mutex);

    atomic_store(&g_ctx.initialized, 0);
    atomic_store(&g_destroyed, 1);

    LOG_DEBUG("FCM: cleanup complete");
}
