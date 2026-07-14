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
#include <stdatomic.h>

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
static int g_diag_available = -1;
static pthread_mutex_t g_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_nl_seq = ATOMIC_VAR_INIT(1);

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
    
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    if (setsockopt(g_diag_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        LOG_WARN("INET_DIAG: setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
    }
    
    struct inet_diag_req_v2 test_req;
    memset(&test_req, 0, sizeof(test_req));
    test_req.sdiag_family = AF_INET;
    test_req.sdiag_protocol = IPPROTO_TCP;
    test_req.idiag_states = (1 << TCP_ESTABLISHED);
    
    struct sockaddr_nl addr;
    char test_buf[1024];
    struct nlmsghdr *nlh = (struct nlmsghdr*)test_buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(test_req));
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = atomic_fetch_add(&g_nl_seq, 1);
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

int inet_diag_available(void) {
    if (g_diag_available < 0) {
        inet_diag_init();
    }
    return g_diag_available == 1;
}

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

static int add_attr(struct nlmsghdr *nlh, int maxlen, int type, const void *data, int alen) {
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;
    int new_len = NLMSG_ALIGN(nlh->nlmsg_len) + len;
    
    if (new_len > maxlen) {
        LOG_ERROR("INET_DIAG: add_attr failed: buffer too small (new_len=%d, maxlen=%d)",
                  new_len, maxlen);
        return -1;
    }
    
    rta = (struct rtattr*)NLMSG_TAIL(nlh);
    rta->rta_type = type;
    rta->rta_len = len;
    
    if (data) {
        memcpy(RTA_DATA(rta), data, alen);
    }
    
    nlh->nlmsg_len = new_len;
    return 0;
}

static int build_uid_filter(struct nlmsghdr *nlh, int maxlen, int uid) {
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 52),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, uid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 65535),
        BPF_STMT(BPF_RET | BPF_K, 0),
    };

    if (add_attr(nlh, maxlen, INET_DIAG_REQ_BYTECODE, code, sizeof(code)) != 0) {
        LOG_ERROR("INET_DIAG: build_uid_filter failed");
        return -1;
    }
    return 0;
}

static int send_diag_request(struct inet_diag_req_v2 *req, char **response, size_t *resp_len, int uid) {
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh;
    char buf[8192];
    int ret = -1;
    
    if (g_diag_sock < 0) {
        return -1;
    }
    
    nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*req));
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = atomic_fetch_add(&g_nl_seq, 1);
    nlh->nlmsg_pid = getpid();
    
    memcpy(NLMSG_DATA(nlh), req, sizeof(*req));
    
    if (uid > 0) {
        if (build_uid_filter(nlh, (int)sizeof(buf), uid) != 0) {
            return -1;
        }
    }
    
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
        
        struct nlmsghdr *nh;
        size_t remaining = (size_t)len;
        for (nh = (struct nlmsghdr*)recv_buf; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (nh->nlmsg_type == NLMSG_DONE) {
                goto done;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nh);
                LOG_DEBUG("Netlink error: %d", err->error);
                goto done;
            }
            
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

static int parse_diag_message_v6(struct nlmsghdr *nlh, connection_info_t *conn) {
    struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nlh);
    
    memset(conn, 0, sizeof(connection_info_t));
    
    conn->uid = diag->idiag_uid;
    conn->state = diag->idiag_state;
    conn->family = AF_INET6;
    
    memcpy(conn->src.v6.ip, diag->id.idiag_src, 16);
    memcpy(conn->dst.v6.ip, diag->id.idiag_dst, 16);
    conn->src_port = ntohs(diag->id.idiag_sport);
    conn->dst_port = ntohs(diag->id.idiag_dport);
    
    inet_ntop(AF_INET6, conn->src.v6.ip, conn->src_ip_str, sizeof(conn->src_ip_str));
    inet_ntop(AF_INET6, conn->dst.v6.ip, conn->dst_ip_str, sizeof(conn->dst_ip_str));
    
    return 0;
}

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
    
    if (send_diag_request(&req, &response, &resp_len, -1) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
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

int inet_diag_get_uid_v6(int protocol,
                          const uint8_t *src_ip, uint16_t src_port,
                          const uint8_t *dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    int uid = -1;
    
    if (!inet_diag_available()) {
        return -1;
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
    
    if (send_diag_request(&req, &response, &resp_len, -1) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
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

int inet_diag_get_uid_fallback(int family, int protocol,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port) {
    const char *proc_path = (family == AF_INET) ? "/proc/net/tcp" : "/proc/net/tcp6";
    FILE *fp = fopen(proc_path, "r");
    char line[512];
    int found_uid = -1;
    
    (void)protocol;
    
    if (!fp) {
        LOG_DEBUG("Cannot open %s for fallback lookup", proc_path);
        return -1;
    }
    
    /* Convert to network byte order for comparison with /proc */
    uint32_t src_ip_net = htonl(src_ip);
    uint32_t dst_ip_net = htonl(dst_ip);
    
    /* Skip header line */
    fgets(line, sizeof(line), fp);
    
    while (fgets(line, sizeof(line), fp)) {
        char tokens[16][64];
        int token_count = 0;
        char *saveptr;
        char *tok = strtok_r(line, " \t\r\n", &saveptr);
        
        while (tok && token_count < 16) {
            strncpy(tokens[token_count], tok, sizeof(tokens[0]) - 1);
            tokens[token_count][sizeof(tokens[0]) - 1] = '\0';
            token_count++;
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
        }
        
        if (token_count < 8) continue;
        
        char *local = tokens[1];
        char *remote = tokens[2];
        char *uid_str = tokens[7];
        
        uint32_t l_ip, r_ip;
        uint16_t l_port, r_port;
        
        if (sscanf(local, "%x:%hx", &l_ip, &l_port) != 2) continue;
        if (sscanf(remote, "%x:%hx", &r_ip, &r_port) != 2) continue;
        
        /* Compare in network byte order */
        if (l_ip == src_ip_net && l_port == src_port &&
            r_ip == dst_ip_net && r_port == dst_port) {
            found_uid = atoi(uid_str);
            break;
        }
    }
    
    fclose(fp);
    return found_uid;
}

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
    
    if (filter && filter->family == AF_INET) {
        families[0] = AF_INET;
        num_families = 1;
    } else if (filter && filter->family == AF_INET6) {
        families[0] = AF_INET6;
        num_families = 1;
    }
    
    int filter_uid = (filter && filter->uid > 0) ? filter->uid : -1;
    
    pthread_mutex_lock(&g_diag_mutex);
    
    for (int f = 0; f < num_families; f++) {
        memset(&req, 0, sizeof(req));
        req.sdiag_family = families[f];
        req.sdiag_protocol = (filter && filter->protocol > 0) ? filter->protocol : IPPROTO_TCP;
        req.idiag_states = (filter && filter->state_mask) ? filter->state_mask : (1 << TCP_ESTABLISHED);
        
        if (send_diag_request(&req, &response, &resp_len, filter_uid) == 0 && response) {
            struct nlmsghdr *nh;
            size_t remaining = resp_len;
            for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
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
                
                if (filter) {
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

int inet_diag_get_connections_by_uid(int uid, connection_info_t **conns, int *count) {
    inet_diag_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.uid = uid;
    filter.protocol = IPPROTO_TCP;
    filter.state_mask = (1 << TCP_ESTABLISHED);
    
    return inet_diag_get_connections_filtered(conns, count, &filter);
}

void inet_diag_free_connections(connection_info_t *conns) {
    if (conns) free(conns);
}

int inet_diag_is_port_owned(int port, int protocol, int uid) {
    connection_info_t *conns = NULL;
    int count = 0;
    int found = 0;
    
    (void)protocol;
    
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
    
    if (send_diag_request(&req, &response, &resp_len, -1) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
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
