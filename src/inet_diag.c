#include "inet_diag.h"
#include "logger.h"
#include "utils.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/inet_diag.h>
#include <linux/rtnetlink.h>
#include <linux/filter.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>
#include <ctype.h>

#define INET_DIAG_SOCKET_TIMEOUT_MS 3000
#define NLMSG_TAIL(nmsg) ((struct rtattr*)(((char*)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len))

/* Fallback definitions for older kernels */
#ifndef SOCK_DIAG_BY_FAMILY
#define SOCK_DIAG_BY_FAMILY 20
#endif

#ifndef INET_DIAG_REQ_BYTECODE
#define INET_DIAG_REQ_BYTECODE 1
#endif

/* Netlink socket for INET_DIAG */
static int g_diag_sock = -1;
static int g_diag_available = -1;  /* -1=unknown, 0=unavailable, 1=available */
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
        g_diag_available = 0;
        pthread_mutex_unlock(&g_diag_mutex);
        return -1;
    }
    
    /* Set receive timeout */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(g_diag_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    /* Test if socket works (SELinux may block it) */
    struct inet_diag_req_v2 test_req;
    memset(&test_req, 0, sizeof(test_req));
    test_req.sdiag_family = AF_INET;
    test_req.sdiag_protocol = IPPROTO_TCP;
    test_req.idiag_states = (1 << TCP_ESTABLISHED);
    
    /* Quick test: send a small request */
    struct sockaddr_nl addr;
    char test_buf[1024];
    struct nlmsghdr *nlh = (struct nlmsghdr*)test_buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(test_req));
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = 1;
    nlh->nlmsg_pid = getpid();
    memcpy(NLMSG_DATA(nlh), &test_req, sizeof(test_req));
    
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
        if (errno == EACCES || errno == EPERM) {
            LOG_WARN("INET_DIAG blocked by SELinux: %s", strerror(errno));
            LOG_WARN("Run: magiskpolicy --live \"allow atpd self netlink_tcpdiag_socket { create read write nlmsg_read }\"");
        } else {
            LOG_WARN("INET_DIAG test failed: %s", strerror(errno));
        }
        close(g_diag_sock);
        g_diag_sock = -1;
        g_diag_available = 0;
        pthread_mutex_unlock(&g_diag_mutex);
        return -1;
    }
    
    g_diag_available = 1;
    LOG_DEBUG("INET_DIAG module initialized (SELinux OK)");
    pthread_mutex_unlock(&g_diag_mutex);
    return 0;
}

/* Check if INET_DIAG is available */
int inet_diag_available(void) {
    if (g_diag_available < 0) {
        inet_diag_init();
    }
    return g_diag_available == 1;
}

/* Cleanup */
void inet_diag_cleanup(void) {
    pthread_mutex_lock(&g_diag_mutex);
    if (g_diag_sock >= 0) {
        close(g_diag_sock);
        g_diag_sock = -1;
    }
    g_diag_available = -1;
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

/* Build bytecode filter for UID filtering */
static int build_uid_filter(struct inet_diag_req_v2 *req, int uid) {
    /* Simplified: we'll filter in userspace for now */
    /* Full BPF implementation would go here */
    (void)req;
    (void)uid;
    return 0;
}

/* Send diag request and receive response */
static int send_diag_request(struct inet_diag_req_v2 *req, char **response, size_t *resp_len) {
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh;
    char buf[8192];
    int ret = -1;
    
    if (g_diag_sock < 0) {
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

/* Parse diag message to extract UID (IPv4) */
static int parse_diag_message_v4(struct nlmsghdr *nlh, connection_info_t *conn) {
    struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nlh);
    
    memset(conn, 0, sizeof(connection_info_t));
    
    conn->uid = diag->idiag_uid;
    conn->state = diag->idiag_state;
    conn->family = AF_INET;
    
    conn->src.v4.ip = diag->id.idiag_src[0];
    conn->dst.v4.ip = diag->id.idiag_dst[0];
    conn->src_port = ntohs(diag->id.idiag_sport);
    conn->dst_port = ntohs(diag->id.idiag_dport);
    
    inet_ntop(AF_INET, &conn->src.v4.ip, conn->src_ip_str, sizeof(conn->src_ip_str));
    inet_ntop(AF_INET, &conn->dst.v4.ip, conn->dst_ip_str, sizeof(conn->dst_ip_str));
    
    return 0;
}

/* Parse diag message to extract UID (IPv6) */
static int parse_diag_message_v6(struct nlmsghdr *nlh, connection_info_t *conn) {
    struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nlh);
    
    memset(conn, 0, sizeof(connection_info_t));
    
    conn->uid = diag->idiag_uid;
    conn->state = diag->idiag_state;
    conn->family = AF_INET6;
    
    /* IPv6 address is stored in idiag_src[0-3] as 4x32bit */
    memcpy(conn->src.v6.ip, diag->id.idiag_src, 16);
    memcpy(conn->dst.v6.ip, diag->id.idiag_dst, 16);
    conn->src_port = ntohs(diag->id.idiag_sport);
    conn->dst_port = ntohs(diag->id.idiag_dport);
    
    inet_ntop(AF_INET6, conn->src.v6.ip, conn->src_ip_str, sizeof(conn->src_ip_str));
    inet_ntop(AF_INET6, conn->dst.v6.ip, conn->dst_ip_str, sizeof(conn->dst_ip_str));
    
    return 0;
}

/* Get UID for a specific connection (IPv4) */
int inet_diag_get_uid_v4(int protocol,
                          uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    int uid = -1;
    
    if (!inet_diag_available()) {
        return inet_diag_get_uid_fallback(AF_INET, protocol, src_ip, src_port, dst_ip, dst_port);
    }
    
    pthread_mutex_lock(&g_diag_mutex);
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = AF_INET;
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
            uid = diag->idiag_uid;
            break;
        }
        free(response);
    }
    
    pthread_mutex_unlock(&g_diag_mutex);
    return uid;
}

/* Get UID for a specific connection (IPv6) */
int inet_diag_get_uid_v6(int protocol,
                          const uint8_t *src_ip, uint16_t src_port,
                          const uint8_t *dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    int uid = -1;
    
    if (!inet_diag_available()) {
        return -1;  /* Fallback not easily implemented for IPv6 */
    }
    
    pthread_mutex_lock(&g_diag_mutex);
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = AF_INET6;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    memcpy(req.id.idiag_src, src_ip, 16);
    memcpy(req.id.idiag_dst, dst_ip, 16);
    
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

/* Fallback: get UID from /proc/net/tcp (works without INET_DIAG) */
int inet_diag_get_uid_fallback(int family, int protocol,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port) {
    const char *proc_path = (family == AF_INET) ? "/proc/net/tcp" : "/proc/net/tcp6";
    FILE *fp = fopen(proc_path, "r");
    char line[512];
    char src_hex[32], dst_hex[32];
    int found_uid = -1;
    
    (void)protocol;
    
    if (!fp) {
        LOG_DEBUG("Cannot open %s for fallback lookup", proc_path);
        return -1;
    }
    
    /* Format: local_address:port remote_address:port ... uid: ... */
    snprintf(src_hex, sizeof(src_hex), "%08X:%04X", ntohl(src_ip), src_port);
    snprintf(dst_hex, sizeof(dst_hex), "%08X:%04X", ntohl(dst_ip), dst_port);
    
    /* Skip header line */
    fgets(line, sizeof(line), fp);
    
    while (fgets(line, sizeof(line), fp)) {
        char local[32], remote[32];
        int uid;
        
        if (sscanf(line, "%*d: %31s %31s %*X %*X %*d %d", local, remote, &uid) >= 3) {
            if (strcmp(local, src_hex) == 0 && strcmp(remote, dst_hex) == 0) {
                found_uid = uid;
                break;
            }
        }
    }
    
    fclose(fp);
    return found_uid;
}

/* Get all connections with filtering */
int inet_diag_get_connections_filtered(connection_info_t **conns, int *count,
                                         inet_diag_filter_t *filter) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    connection_info_t *list = NULL;
    int list_size = 0;
    int list_capacity = 0;
    int families[] = {AF_INET, AF_INET6};
    int num_families = 2;
    
    if (!inet_diag_available()) {
        return -1;
    }
    
    /* If specific family requested, only scan that */
    if (filter && filter->family == AF_INET) {
        families[0] = AF_INET;
        num_families = 1;
    } else if (filter && filter->family == AF_INET6) {
        families[0] = AF_INET6;
        num_families = 1;
    }
    
    pthread_mutex_lock(&g_diag_mutex);
    
    for (int f = 0; f < num_families; f++) {
        memset(&req, 0, sizeof(req));
        req.sdiag_family = families[f];
        req.sdiag_protocol = (filter && filter->protocol > 0) ? filter->protocol : IPPROTO_TCP;
        req.idiag_states = (filter && filter->state_mask) ? filter->state_mask : (1 << TCP_ESTABLISHED);
        
        if (filter && filter->uid > 0) {
            build_uid_filter(&req, filter->uid);
        }
        
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
                
                if (families[f] == AF_INET) {
                    parse_diag_message_v4(nh, &list[list_size]);
                } else {
                    parse_diag_message_v6(nh, &list[list_size]);
                }
                
                /* Apply filter */
                if (filter) {
                    if (filter->uid > 0 && list[list_size].uid != filter->uid) {
                        continue;
                    }
                    if (filter->src_port > 0 && list[list_size].src_port != filter->src_port) {
                        continue;
                    }
                    if (filter->dst_port > 0 && list[list_size].dst_port != filter->dst_port) {
                        continue;
                    }
                }
                
                list_size++;
            }
            free(response);
            response = NULL;
        }
    }
    
    pthread_mutex_unlock(&g_diag_mutex);
    
    *conns = list;
    *count = list_size;
    return 0;
}

/* Get connections for a specific UID */
int inet_diag_get_connections_by_uid(int uid, connection_info_t **conns, int *count) {
    inet_diag_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.uid = uid;
    filter.protocol = IPPROTO_TCP;
    filter.state_mask = (1 << TCP_ESTABLISHED);
    
    return inet_diag_get_connections_filtered(conns, count, &filter);
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
    
    if (inet_diag_get_connections_filtered(&conns, &count, NULL) != 0) {
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

/* Get socket inode for a connection (for debugging) */
uint32_t inet_diag_get_socket_inode(int family, int protocol,
                                     uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    uint32_t inode = 0;
    
    if (!inet_diag_available()) {
        return 0;
    }
    
    pthread_mutex_lock(&g_diag_mutex);
    
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
