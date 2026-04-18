#include "inet_diag.h"
#include "logger.h"
#include "utils.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/inet_diag.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define INET_DIAG_SOCKET_TIMEOUT_MS 3000
#define NLMSG_TAIL(nmsg) ((struct rtattr*)(((char*)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

/* Fallback definitions for older kernels */
#ifndef SOCK_DIAG_BY_FAMILY
#define SOCK_DIAG_BY_FAMILY 20
#endif

#ifndef INET_DIAG_REQ_BYTECODE
#define INET_DIAG_REQ_BYTECODE 1
#endif

/* Netlink socket for INET_DIAG */
static int g_diag_sock = -1;
static pthread_mutex_t g_diag_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Initialize Netlink socket for INET_DIAG */
int inet_diag_init(void) {
    pthread_mutex_lock(&g_diag_mutex);
    
    if (g_diag_sock >= 0) {
        pthread_mutex_unlock(&g_diag_mutex);
        return 0;
    }
    
    g_diag_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_INET_DIAG);
    if (g_diag_sock < 0) {
        LOG_ERROR("Failed to create INET_DIAG socket: %s", strerror(errno));
        pthread_mutex_unlock(&g_diag_mutex);
        return -1;
    }
    
    /* Set receive timeout */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(g_diag_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    LOG_DEBUG("INET_DIAG module initialized");
    pthread_mutex_unlock(&g_diag_mutex);
    return 0;
}

/* Cleanup */
void inet_diag_cleanup(void) {
    pthread_mutex_lock(&g_diag_mutex);
    if (g_diag_sock >= 0) {
        close(g_diag_sock);
        g_diag_sock = -1;
    }
    pthread_mutex_unlock(&g_diag_mutex);
    LOG_DEBUG("INET_DIAG module cleaned up");
}

/* Add attribute to netlink message */
static void add_attr(struct nlmsghdr *nlh, int maxlen, int type, const void *data, int alen) {
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;
    int new_len = NLMSG_ALIGN(nlh->nlmsg_len) + len;
    
    if (new_len > maxlen) {
        return;
    }
    
    rta = (struct rtattr*)NLMSG_TAIL(nlh);
    rta->rta_type = type;
    rta->rta_len = len;
    
    if (data) {
        memcpy(RTA_DATA(rta), data, alen);
    }
    
    nlh->nlmsg_len = new_len;
}

/* Send diag request and receive response */
static int send_diag_request(struct inet_diag_req_v2 *req, char **response, size_t *resp_len) {
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh;
    char buf[8192];
    int ret = -1;
    
    if (g_diag_sock < 0) {
        LOG_ERROR("INET_DIAG socket not initialized");
        return -1;
    }
    
    /* Build netlink message */
    nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*req));
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = time(NULL);
    nlh->nlmsg_pid = getpid();
    
    memcpy(NLMSG_DATA(nlh), req, sizeof(*req));
    
    /* Send request */
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    struct iovec iov = { nlh, nlh->nlmsg_len };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    if (sendmsg(g_diag_sock, &msg, 0) < 0) {
        LOG_DEBUG("Failed to send diag request: %s", strerror(errno));
        return -1;
    }
    
    /* Receive response (multiple messages) */
    size_t total_len = 0;
    char *resp_data = NULL;
    
    while (1) {
        char recv_buf[16384];
        struct iovec recv_iov = { recv_buf, sizeof(recv_buf) };
        struct msghdr recv_msg = {
            .msg_name = &addr,
            .msg_namelen = sizeof(addr),
            .msg_iov = &recv_iov,
            .msg_iovlen = 1
        };
        
        ssize_t len = recvmsg(g_diag_sock, &recv_msg, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_DEBUG("recvmsg error: %s", strerror(errno));
            break;
        }
        
        /* Process netlink messages */
        struct nlmsghdr *nh;
        for (nh = (struct nlmsghdr*)recv_buf; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE) {
                goto done;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nh);
                LOG_DEBUG("Netlink error: %d", err->error);
                goto done;
            }
            
            /* Append to response */
            size_t msg_len = nh->nlmsg_len;
            char *new_resp = realloc(resp_data, total_len + msg_len);
            if (!new_resp) {
                free(resp_data);
                return -1;
            }
            resp_data = new_resp;
            memcpy(resp_data + total_len, nh, msg_len);
            total_len += msg_len;
        }
    }
    
done:
    if (total_len > 0) {
        *response = resp_data;
        *resp_len = total_len;
        ret = 0;
    } else {
        free(resp_data);
        ret = -1;
    }
    
    return ret;
}

/* Parse diag message to extract UID */
static int parse_diag_message(struct nlmsghdr *nlh, connection_info_t *conn) {
    struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*diag));
    
    memset(conn, 0, sizeof(connection_info_t));
    
    conn->uid = diag->idiag_uid;
    conn->state = diag->idiag_state;
    conn->family = diag->idiag_family;
    
    /* Extract addresses */
    if (conn->family == AF_INET) {
        conn->src_ip = diag->id.idiag_src[0];
        conn->dst_ip = diag->id.idiag_dst[0];
        conn->src_port = ntohs(diag->id.idiag_sport);
        conn->dst_port = ntohs(diag->id.idiag_dport);
        
        inet_ntop(AF_INET, &conn->src_ip, conn->src_ip_str, sizeof(conn->src_ip_str));
        inet_ntop(AF_INET, &conn->dst_ip, conn->dst_ip_str, sizeof(conn->dst_ip_str));
    }
    
    /* Parse extended attributes (socket inode, etc.) */
    for (rta = (struct rtattr*)(diag + 1); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == INET_DIAG_INFO) {
            /* TCP info - not needed for UID */
        } else if (rta->rta_type == INET_DIAG_CONG) {
            /* Congestion control name */
        }
    }
    
    return 0;
}

/* Get UID for a specific connection */
int inet_diag_get_uid(int family, int protocol,
                       uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    int uid = -1;
    
    pthread_mutex_lock(&g_diag_mutex);
    
    if (g_diag_sock < 0) {
        pthread_mutex_unlock(&g_diag_mutex);
        return -1;
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = family;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    
    /* Set socket identifiers for lookup */
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    req.id.idiag_src[0] = src_ip;
    req.id.idiag_dst[0] = dst_ip;
    
    if (send_diag_request(&req, &response, &resp_len) == 0 && response) {
        struct nlmsghdr *nh;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, resp_len); nh = NLMSG_NEXT(nh, resp_len)) {
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nh);
            uid = diag->idiag_uid;
            break;
        }
        free(response);
    }
    
    pthread_mutex_unlock(&g_diag_mutex);
    return uid;
}

/* Get all connections matching filter */
int inet_diag_get_connections(connection_info_t **conns, int *count, int protocol, int state_mask) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    connection_info_t *list = NULL;
    int list_size = 0;
    int list_capacity = 0;
    
    pthread_mutex_lock(&g_diag_mutex);
    
    if (g_diag_sock < 0) {
        pthread_mutex_unlock(&g_diag_mutex);
        return -1;
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = AF_INET;
    req.sdiag_protocol = protocol;
    req.idiag_states = state_mask ? state_mask : (1 << TCP_ESTABLISHED);
    
    if (send_diag_request(&req, &response, &resp_len) == 0 && response) {
        struct nlmsghdr *nh;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, resp_len); nh = NLMSG_NEXT(nh, resp_len)) {
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            if (list_size >= list_capacity) {
                list_capacity = list_capacity ? list_capacity * 2 : 64;
                connection_info_t *new_list = realloc(list, sizeof(connection_info_t) * list_capacity);
                if (!new_list) {
                    free(list);
                    free(response);
                    pthread_mutex_unlock(&g_diag_mutex);
                    return -1;
                }
                list = new_list;
            }
            
            parse_diag_message(nh, &list[list_size]);
            list_size++;
        }
        free(response);
    }
    
    pthread_mutex_unlock(&g_diag_mutex);
    
    *conns = list;
    *count = list_size;
    return 0;
}

/* Get connections for a specific UID */
int inet_diag_get_connections_by_uid(int uid, connection_info_t **conns, int *count) {
    connection_info_t *all_conns = NULL;
    connection_info_t *filtered = NULL;
    int all_count = 0;
    int filtered_count = 0;
    int filtered_capacity = 0;
    
    if (inet_diag_get_connections(&all_conns, &all_count, DIAG_PROTO_TCP, 0) != 0) {
        return -1;
    }
    
    for (int i = 0; i < all_count; i++) {
        if (all_conns[i].uid == uid) {
            if (filtered_count >= filtered_capacity) {
                filtered_capacity = filtered_capacity ? filtered_capacity * 2 : 16;
                connection_info_t *new_filtered = realloc(filtered, sizeof(connection_info_t) * filtered_capacity);
                if (!new_filtered) {
                    free(all_conns);
                    free(filtered);
                    return -1;
                }
                filtered = new_filtered;
            }
            memcpy(&filtered[filtered_count], &all_conns[i], sizeof(connection_info_t));
            filtered_count++;
        }
    }
    
    free(all_conns);
    *conns = filtered;
    *count = filtered_count;
    return 0;
}

/* Free connection list */
void inet_diag_free_connections(connection_info_t *conns) {
    if (conns) free(conns);
}

/* Check if a port is being used by a specific UID */
int inet_diag_is_port_owned(int port, int protocol, int uid) {
    connection_info_t *conns = NULL;
    int count = 0;
    int found = 0;
    
    if (inet_diag_get_connections(&conns, &count, protocol, 0) != 0) {
        return 0;
    }
    
    for (int i = 0; i < count; i++) {
        if (conns[i].src_port == port && conns[i].uid == uid) {
            found = 1;
            break;
        }
        if (conns[i].dst_port == port && conns[i].uid == uid) {
            found = 1;
            break;
        }
    }
    
    inet_diag_free_connections(conns);
    return found;
}

/* Get socket inode (for debugging) */
uint32_t inet_diag_get_socket_inode(int family, int protocol,
                                     uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    uint32_t inode = 0;
    
    pthread_mutex_lock(&g_diag_mutex);
    
    if (g_diag_sock < 0) {
        pthread_mutex_unlock(&g_diag_mutex);
        return 0;
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = family;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    req.id.idiag_src[0] = src_ip;
    req.id.idiag_dst[0] = dst_ip;
    
    if (send_diag_request(&req, &response, &resp_len) == 0 && response) {
        struct nlmsghdr *nh;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, resp_len); nh = NLMSG_NEXT(nh, resp_len)) {
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nh);
            inode = diag->idiag_inode;
            break;
        }
        free(response);
    }
    
    pthread_mutex_unlock(&g_diag_mutex);
    return inode;
}
