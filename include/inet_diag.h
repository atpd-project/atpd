#ifndef ATP_INET_DIAG_H
#define ATP_INET_DIAG_H

#include <stdint.h>
#include <netinet/in.h>
#include <string.h>

/* TCP states (same as kernel) */
#define TCP_ESTABLISHED 1
#define TCP_SYN_SENT    2
#define TCP_SYN_RECV    3
#define TCP_FIN_WAIT1   4
#define TCP_FIN_WAIT2   5
#define TCP_TIME_WAIT   6
#define TCP_CLOSE       7
#define TCP_CLOSE_WAIT  8
#define TCP_LAST_ACK    9
#define TCP_LISTEN      10
#define TCP_CLOSING     11
#define TCP_NEW_SYN_RECV 12

/* Protocol types */
typedef enum {
    DIAG_PROTO_TCP = 6,
    DIAG_PROTO_UDP = 17
} diag_protocol_t;

/* Connection information structure */
typedef struct {
    int uid;
    int pid;
    char comm[64];
    union {
        struct {
            uint32_t ip;
        } v4;
        struct {
            uint8_t ip[16];
        } v6;
    } src;
    union {
        struct {
            uint32_t ip;
        } v4;
        struct {
            uint8_t ip[16];
        } v6;
    } dst;
    uint16_t src_port;
    uint16_t dst_port;
    int state;
    int family;  /* AF_INET or AF_INET6 */
    char src_ip_str[INET6_ADDRSTRLEN];
    char dst_ip_str[INET6_ADDRSTRLEN];
} connection_info_t;

/* Connection filter for bytecode filtering */
typedef struct {
    int uid;           /* Filter by UID (-1 = any) */
    int protocol;      /* Filter by protocol (-1 = any) */
    int state_mask;    /* Filter by state mask (0 = all) */
    int family;        /* Filter by family (AF_INET/AF_INET6/0=any) */
    uint32_t src_ip;   /* Source IP (0 = any) */
    uint32_t dst_ip;   /* Destination IP (0 = any) */
    uint16_t src_port; /* Source port (0 = any) */
    uint16_t dst_port; /* Destination port (0 = any) */
} inet_diag_filter_t;

/* Initialize INET_DIAG module */
int inet_diag_init(void);

/* Cleanup INET_DIAG module */
void inet_diag_cleanup(void);

/* Check if INET_DIAG is available (SELinux permissions) */
int inet_diag_available(void);

/* Get UID for a specific connection (IPv4) */
int inet_diag_get_uid_v4(int protocol,
                          uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port);

/* Get UID for a specific connection (IPv6) */
int inet_diag_get_uid_v6(int protocol,
                          const uint8_t *src_ip, uint16_t src_port,
                          const uint8_t *dst_ip, uint16_t dst_port);

/* Get all connections with kernel-side filtering (using bytecode) */
int inet_diag_get_connections_filtered(connection_info_t **conns, int *count,
                                         inet_diag_filter_t *filter);

/* Get connections for a specific UID (using kernel filter) */
int inet_diag_get_connections_by_uid(int uid, connection_info_t **conns, int *count);

/* Get all connections (simplified wrapper) */
static inline int inet_diag_get_connections(connection_info_t **conns, int *count,
                                             int protocol, int state_mask) {
    inet_diag_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.protocol = protocol;
    filter.state_mask = state_mask;
    return inet_diag_get_connections_filtered(conns, count, &filter);
}

/* Fallback: get UID from /proc filesystem (works without INET_DIAG) */
int inet_diag_get_uid_fallback(int family, int protocol,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port);

/* Free connection list */
void inet_diag_free_connections(connection_info_t *conns);

/* Check if a port is being used by a specific UID */
int inet_diag_is_port_owned(int port, int protocol, int uid);

/* Get socket inode for a connection (for debugging) */
uint32_t inet_diag_get_socket_inode(int family, int protocol,
                                     uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port);

#endif
