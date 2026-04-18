#ifndef ATP_INET_DIAG_H
#define ATP_INET_DIAG_H

#include <stdint.h>
#include <netinet/in.h>

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

/* Connection information structure */
typedef struct {
    int uid;
    int pid;
    char comm[64];
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    int state;
    int family;  /* AF_INET or AF_INET6 */
    char src_ip_str[INET_ADDRSTRLEN];
    char dst_ip_str[INET_ADDRSTRLEN];
} connection_info_t;

/* Protocol types */
typedef enum {
    DIAG_PROTO_TCP = 6,
    DIAG_PROTO_UDP = 17
} diag_protocol_t;

/* Initialize INET_DIAG module */
int inet_diag_init(void);

/* Cleanup INET_DIAG module */
void inet_diag_cleanup(void);

/* Get UID for a specific connection */
int inet_diag_get_uid(int family, int protocol,
                       uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port);

/* Get UID by socket inode (alternative method) */
int inet_diag_get_uid_by_inode(uint32_t inode);

/* Get all connections matching filter */
int inet_diag_get_connections(connection_info_t **conns, int *count, int protocol, int state_mask);

/* Get connections for a specific UID */
int inet_diag_get_connections_by_uid(int uid, connection_info_t **conns, int *count);

/* Free connection list */
void inet_diag_free_connections(connection_info_t *conns);

/* Check if a port is being used by a specific UID */
int inet_diag_is_port_owned(int port, int protocol, int uid);

/* Get socket inode for a connection (for debugging) */
uint32_t inet_diag_get_socket_inode(int family, int protocol,
                                     uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port);

#endif
