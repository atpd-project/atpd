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
#include <dirent.h>
#include <ctype.h>
#include <stdatomic.h>

#define INET_DIAG_SOCKET_TIMEOUT_MS 3000
#define INET_DIAG_RESPONSE_BUFFER_SIZE (64 * 1024)
#define INET_DIAG_RESPONSE_MAX_SIZE (4 * 1024 * 1024)

/* Fallback definitions for older kernels */
#ifndef SOCK_DIAG_BY_FAMILY
#define SOCK_DIAG_BY_FAMILY 20
#endif

#ifndef INET_DIAG_NOCOOKIE
#define INET_DIAG_NOCOOKIE (~0U)
#endif

/* ========== Global State ========== */

static int g_diag_sock = -1;
static int g_diag_available = -1;
static int g_diag_initialized = 0;
static atomic_int g_diag_destroyed = 0;
static atomic_int g_diag_shutting_down = 0;
static pthread_mutex_t g_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_nl_seq;

static pthread_key_t g_tls_sock_key;
static pthread_once_t g_tls_key_once = PTHREAD_ONCE_INIT;

/* ========== Forward Declarations ========== */

int inet_diag_get_uid_fallback_v4(int protocol,
                                   uint32_t src_ip, uint16_t src_port,
                                   uint32_t dst_ip, uint16_t dst_port);

int inet_diag_get_uid_fallback_v6(int protocol,
                                   const uint8_t *src_ip, uint16_t src_port,
                                   const uint8_t *dst_ip, uint16_t dst_port);

/* ========== TLS Socket Destructor ========== */

static void tls_socket_destructor(void *ptr) {
    int sock = (int)(intptr_t)ptr;
    if (sock >= 0) {
        close(sock);
        LOG_DEBUG("[INET_DIAG] TLS socket %d destroyed", sock);
    }
}

static void tls_key_init(void) {
    pthread_key_create(&g_tls_sock_key, tls_socket_destructor);
}

/* ========== TLS Socket ========== */

static int get_tls_socket(void) {
    pthread_once(&g_tls_key_once, tls_key_init);
    
    int sock = (int)(intptr_t)pthread_getspecific(g_tls_sock_key);
    if (sock > 0) {
        return sock;
    }
    
    sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_INET_DIAG);
    if (sock < 0) {
        LOG_ERROR("[INET_DIAG] TLS socket creation failed: %s", strerror(errno));
        return -1;
    }
    
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;  /* Kernel will assign */
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("[INET_DIAG] TLS bind failed: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    pthread_setspecific(g_tls_sock_key, (void*)(intptr_t)sock);
    LOG_DEBUG("[INET_DIAG] TLS socket %d created", sock);
    return sock;
}

/* ========== Initialization ========== */

static int inet_diag_do_init(void) {
    if (atomic_load(&g_diag_destroyed)) {
        LOG_ERROR("[INET_DIAG] module destroyed, cannot reinit (process restart required)");
        return -1;
    }
    
    atomic_init(&g_nl_seq, 1);
    
    g_diag_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_INET_DIAG);
    if (g_diag_sock < 0) {
        LOG_ERROR("[INET_DIAG] socket creation failed: %s", strerror(errno));
        g_diag_available = 0;
        return -1;
    }
    
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    if (setsockopt(g_diag_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        LOG_WARN("[INET_DIAG] setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
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
            LOG_WARN("[INET_DIAG] blocked by SELinux: %s", strerror(errno));
            LOG_WARN("[INET_DIAG] Run: magiskpolicy --live \"allow atpd self netlink_tcpdiag_socket { create read write nlmsg_read }\"");
        } else {
            LOG_WARN("[INET_DIAG] test failed: %s", strerror(errno));
        }
        close(g_diag_sock);
        g_diag_sock = -1;
        g_diag_available = 0;
        return -1;
    }
    
    g_diag_available = 1;
    g_diag_initialized = 1;
    LOG_DEBUG("[INET_DIAG] initialized (SELinux OK)");
    return 0;
}

int inet_diag_init(void) {
    pthread_mutex_lock(&g_diag_mutex);
    int ret = 0;
    
    if (g_diag_initialized) {
        pthread_mutex_unlock(&g_diag_mutex);
        return 0;
    }
    
    ret = inet_diag_do_init();
    pthread_mutex_unlock(&g_diag_mutex);
    return ret;
}

int inet_diag_available(void) {
    if (!g_diag_initialized) {
        inet_diag_init();
    }
    return g_diag_available == 1;
}

void inet_diag_cleanup(void) {
    atomic_store(&g_diag_shutting_down, 1);
    
    pthread_mutex_lock(&g_diag_mutex);
    
    if (g_diag_sock >= 0) {
        close(g_diag_sock);
        g_diag_sock = -1;
    }
    g_diag_available = -1;
    g_diag_initialized = 0;
    atomic_store(&g_diag_destroyed, 1);
    
    pthread_mutex_unlock(&g_diag_mutex);
    
    atomic_store(&g_diag_shutting_down, 0);
    LOG_DEBUG("[INET_DIAG] cleaned up (cannot reinit)");
}

/* ========== Netlink Helpers ========== */

static inline int is_valid_reply(struct nlmsghdr *nh, uint32_t seq) {
    return nh && nh->nlmsg_seq == seq && nh->nlmsg_pid == 0;
}

static inline int is_v4_match(struct inet_diag_msg *diag,
                               uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port) {
    return diag->id.idiag_src[0] == src_ip &&
           diag->id.idiag_dst[0] == dst_ip &&
           ntohs(diag->id.idiag_sport) == src_port &&
           ntohs(diag->id.idiag_dport) == dst_port;
}

static inline int is_v6_match(struct inet_diag_msg *diag,
                               const uint8_t *src_ip, uint16_t src_port,
                               const uint8_t *dst_ip, uint16_t dst_port) {
    return memcmp(diag->id.idiag_src, src_ip, 16) == 0 &&
           memcmp(diag->id.idiag_dst, dst_ip, 16) == 0 &&
           ntohs(diag->id.idiag_sport) == src_port &&
           ntohs(diag->id.idiag_dport) == dst_port;
}

/* ========== Send/Receive ========== */

static int send_diag_request_exact(struct inet_diag_req_v2 *req, char **response,
                                    size_t *resp_len, uint32_t *out_seq) {
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh;
    char buf[8192];
    int sock = get_tls_socket();
    uint32_t seq;
    int ret = -1;
    
    if (sock < 0 || !response || !resp_len) {
        return -1;
    }
    
    if (atomic_load(&g_diag_shutting_down)) {
        LOG_DEBUG("[INET_DIAG] shutting down, request rejected");
        return -1;
    }
    
    *response = NULL;
    *resp_len = 0;
    
    nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*req));
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh->nlmsg_flags = NLM_F_REQUEST;  /* Exact query, no dump */
    seq = atomic_fetch_add(&g_nl_seq, 1);
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = getpid();
    
    memcpy(NLMSG_DATA(nlh), req, sizeof(*req));
    
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    struct iovec iov = { nlh, nlh->nlmsg_len };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    if (sendmsg(sock, &msg, 0) < 0) {
        LOG_DEBUG("[INET_DIAG] sendmsg failed: %s", strerror(errno));
        return -1;
    }
    
    char *resp_data = malloc(INET_DIAG_RESPONSE_BUFFER_SIZE);
    if (!resp_data) {
        LOG_ERROR("[INET_DIAG] malloc failed");
        return -1;
    }
    size_t resp_capacity = INET_DIAG_RESPONSE_BUFFER_SIZE;
    size_t total_len = 0;
    int done = 0;
    
    while (!done) {
        char recv_buf[16384];
        struct iovec recv_iov = { recv_buf, sizeof(recv_buf) };
        struct msghdr recv_msg = {
            .msg_name = &addr,
            .msg_namelen = sizeof(addr),
            .msg_iov = &recv_iov,
            .msg_iovlen = 1
        };
        
        ssize_t len = recvmsg(sock, &recv_msg, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_DEBUG("[INET_DIAG] recvmsg error: %s", strerror(errno));
            break;
        }
        
        if (recv_msg.msg_flags & MSG_TRUNC) {
            LOG_WARN("[INET_DIAG] message truncated");
            free(resp_data);
            return -1;
        }
        
        struct nlmsghdr *nh;
        size_t remaining = (size_t)len;
        for (nh = (struct nlmsghdr*)recv_buf; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (!is_valid_reply(nh, seq)) {
                continue;
            }
            
            if (nh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nh);
                LOG_DEBUG("[INET_DIAG] netlink error: %d", err->error);
                done = 1;
                break;
            }
            
            if (nh->nlmsg_type != SOCK_DIAG_BY_FAMILY) {
                continue;
            }
            
            size_t msg_len = nh->nlmsg_len;
            if (total_len + msg_len > resp_capacity) {
                size_t new_cap = resp_capacity * 2;
                if (new_cap > INET_DIAG_RESPONSE_MAX_SIZE) {
                    LOG_WARN("[INET_DIAG] response too large");
                    free(resp_data);
                    return -1;
                }
                char *new_resp = realloc(resp_data, new_cap);
                if (!new_resp) {
                    free(resp_data);
                    return -1;
                }
                resp_data = new_resp;
                resp_capacity = new_cap;
            }
            memcpy(resp_data + total_len, nh, msg_len);
            total_len += msg_len;
        }
    }
    
    if (out_seq) {
        *out_seq = seq;
    }
    
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

static int send_diag_request_dump(struct inet_diag_req_v2 *req, char **response,
                                    size_t *resp_len, uint32_t *out_seq) {
    struct sockaddr_nl addr;
    struct nlmsghdr *nlh;
    char buf[8192];
    int sock = get_tls_socket();
    uint32_t seq;
    int ret = -1;
    
    if (sock < 0 || !response || !resp_len) {
        return -1;
    }
    
    if (atomic_load(&g_diag_shutting_down)) {
        LOG_DEBUG("[INET_DIAG] shutting down, request rejected");
        return -1;
    }
    
    *response = NULL;
    *resp_len = 0;
    
    nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*req));
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    seq = atomic_fetch_add(&g_nl_seq, 1);
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_pid = getpid();
    
    memcpy(NLMSG_DATA(nlh), req, sizeof(*req));
    
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    struct iovec iov = { nlh, nlh->nlmsg_len };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    if (sendmsg(sock, &msg, 0) < 0) {
        LOG_DEBUG("[INET_DIAG] sendmsg failed: %s", strerror(errno));
        return -1;
    }
    
    char *resp_data = malloc(INET_DIAG_RESPONSE_BUFFER_SIZE);
    if (!resp_data) {
        LOG_ERROR("[INET_DIAG] malloc failed");
        return -1;
    }
    size_t resp_capacity = INET_DIAG_RESPONSE_BUFFER_SIZE;
    size_t total_len = 0;
    int done = 0;
    
    while (!done) {
        char recv_buf[16384];
        struct iovec recv_iov = { recv_buf, sizeof(recv_buf) };
        struct msghdr recv_msg = {
            .msg_name = &addr,
            .msg_namelen = sizeof(addr),
            .msg_iov = &recv_iov,
            .msg_iovlen = 1
        };
        
        ssize_t len = recvmsg(sock, &recv_msg, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_DEBUG("[INET_DIAG] recvmsg error: %s", strerror(errno));
            break;
        }
        
        if (recv_msg.msg_flags & MSG_TRUNC) {
            LOG_WARN("[INET_DIAG] message truncated");
            free(resp_data);
            return -1;
        }
        
        struct nlmsghdr *nh;
        size_t remaining = (size_t)len;
        for (nh = (struct nlmsghdr*)recv_buf; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (!is_valid_reply(nh, seq)) {
                continue;
            }
            
            if (nh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nh);
                LOG_DEBUG("[INET_DIAG] netlink error: %d", err->error);
                done = 1;
                break;
            }
            
            if (nh->nlmsg_type != SOCK_DIAG_BY_FAMILY) {
                continue;
            }
            
            size_t msg_len = nh->nlmsg_len;
            if (total_len + msg_len > resp_capacity) {
                size_t new_cap = resp_capacity * 2;
                if (new_cap > INET_DIAG_RESPONSE_MAX_SIZE) {
                    LOG_WARN("[INET_DIAG] response too large");
                    free(resp_data);
                    return -1;
                }
                char *new_resp = realloc(resp_data, new_cap);
                if (!new_resp) {
                    free(resp_data);
                    return -1;
                }
                resp_data = new_resp;
                resp_capacity = new_cap;
            }
            memcpy(resp_data + total_len, nh, msg_len);
            total_len += msg_len;
        }
    }
    
    if (out_seq) {
        *out_seq = seq;
    }
    
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

/* ========== Message Parsing ========== */

static int parse_diag_message_v4(struct nlmsghdr *nlh, connection_info_t *conn) {
    if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct inet_diag_msg)) {
        LOG_ERROR("[INET_DIAG] v4 message too short");
        return -1;
    }
    
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
    if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct inet_diag_msg)) {
        LOG_ERROR("[INET_DIAG] v6 message too short");
        return -1;
    }
    
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

/* ========== Public API ========== */

int inet_diag_get_uid_v4(int protocol,
                          uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    int uid = -1;
    uint32_t seq = 0;
    int ret = -1;
    
    if (!inet_diag_available()) {
        return inet_diag_get_uid_fallback_v4(protocol, src_ip, src_port, dst_ip, dst_port);
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = AF_INET;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
    
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    req.id.idiag_src[0] = src_ip;
    req.id.idiag_dst[0] = dst_ip;
    
    if (send_diag_request_exact(&req, &response, &resp_len, &seq) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (!is_valid_reply(nh, seq)) continue;
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nh);
            if (is_v4_match(diag, src_ip, src_port, dst_ip, dst_port)) {
                uid = diag->idiag_uid;
                break;
            }
        }
        ret = uid;
    }
    
    free(response);
    return ret;
}

int inet_diag_get_uid_v6(int protocol,
                          const uint8_t *src_ip, uint16_t src_port,
                          const uint8_t *dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    int uid = -1;
    uint32_t seq = 0;
    int ret = -1;
    
    if (!inet_diag_available()) {
        return -1;
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = AF_INET6;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
    
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    memcpy(req.id.idiag_src, src_ip, 16);
    memcpy(req.id.idiag_dst, dst_ip, 16);
    
    if (send_diag_request_exact(&req, &response, &resp_len, &seq) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (!is_valid_reply(nh, seq)) continue;
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nh);
            if (is_v6_match(diag, src_ip, src_port, dst_ip, dst_port)) {
                uid = diag->idiag_uid;
                break;
            }
        }
        ret = uid;
    }
    
    free(response);
    return ret;
}

/* ========== Fallback UID Lookup ========== */

int inet_diag_get_uid_fallback_v4(int protocol,
                                   uint32_t src_ip, uint16_t src_port,
                                   uint32_t dst_ip, uint16_t dst_port) {
    FILE *fp = fopen("/proc/net/tcp", "r");
    char line[512];
    int found_uid = -1;
    
    (void)protocol;
    
    if (!fp) {
        LOG_DEBUG("[INET_DIAG] cannot open /proc/net/tcp");
        return -1;
    }
    
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
        
        if (l_ip == src_ip && l_port == src_port &&
            r_ip == dst_ip && r_port == dst_port) {
            found_uid = atoi(uid_str);
            break;
        }
    }
    
    fclose(fp);
    return found_uid;
}

int inet_diag_get_uid_fallback_v6(int protocol,
                                   const uint8_t *src_ip, uint16_t src_port,
                                   const uint8_t *dst_ip, uint16_t dst_port) {
    FILE *fp = fopen("/proc/net/tcp6", "r");
    char line[512];
    int found_uid = -1;
    
    (void)protocol;
    
    if (!fp) {
        LOG_DEBUG("[INET_DIAG] cannot open /proc/net/tcp6");
        return -1;
    }
    
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
        
        char local_ip_hex[33], remote_ip_hex[33];
        uint16_t l_port, r_port;
        int local_parsed, remote_parsed;
        
        local_parsed = sscanf(local, "%32[0-9A-Fa-f]:%hx", local_ip_hex, &l_port);
        remote_parsed = sscanf(remote, "%32[0-9A-Fa-f]:%hx", remote_ip_hex, &r_port);
        
        if (local_parsed != 2 || remote_parsed != 2) continue;
        local_ip_hex[32] = '\0';
        remote_ip_hex[32] = '\0';
        
        uint8_t l_bytes[16], r_bytes[16];
        int valid = 1;
        
        for (int i = 0; i < 32 && valid; i += 2) {
            char byte_str[3] = {local_ip_hex[i], local_ip_hex[i+1], '\0'};
            char *endptr;
            long val = strtol(byte_str, &endptr, 16);
            if (endptr == byte_str || *endptr != '\0' || val < 0 || val > 255) {
                valid = 0;
                break;
            }
            l_bytes[i/2] = (uint8_t)val;
            
            char byte_str_r[3] = {remote_ip_hex[i], remote_ip_hex[i+1], '\0'};
            val = strtol(byte_str_r, &endptr, 16);
            if (endptr == byte_str_r || *endptr != '\0' || val < 0 || val > 255) {
                valid = 0;
                break;
            }
            r_bytes[i/2] = (uint8_t)val;
        }
        
        if (!valid) continue;
        
        if (memcmp(l_bytes, src_ip, 16) == 0 && l_port == src_port &&
            memcmp(r_bytes, dst_ip, 16) == 0 && r_port == dst_port) {
            found_uid = atoi(uid_str);
            break;
        }
    }
    
    fclose(fp);
    return found_uid;
}

/* ========== Socket Inode ========== */

uint32_t inet_diag_get_socket_inode_v6(int protocol,
                                        const uint8_t *src_ip, uint16_t src_port,
                                        const uint8_t *dst_ip, uint16_t dst_port) {
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    uint32_t inode = 0;
    uint32_t seq = 0;
    uint32_t ret = 0;
    
    if (!inet_diag_available()) {
        return 0;
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = AF_INET6;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
    
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    memcpy(req.id.idiag_src, src_ip, 16);
    memcpy(req.id.idiag_dst, dst_ip, 16);
    
    if (send_diag_request_exact(&req, &response, &resp_len, &seq) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (!is_valid_reply(nh, seq)) continue;
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nh);
            if (is_v6_match(diag, src_ip, src_port, dst_ip, dst_port)) {
                inode = diag->idiag_inode;
                break;
            }
        }
        ret = inode;
    }
    
    free(response);
    return ret;
}

uint32_t inet_diag_get_socket_inode(int family, int protocol,
                                     uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port) {
    if (family == AF_INET6) {
        LOG_WARN("[INET_DIAG] IPv6 inode query via legacy API not supported");
        return 0;
    }
    
    struct inet_diag_req_v2 req;
    char *response = NULL;
    size_t resp_len = 0;
    uint32_t inode = 0;
    uint32_t seq = 0;
    uint32_t ret = 0;
    
    if (!inet_diag_available()) {
        return 0;
    }
    
    memset(&req, 0, sizeof(req));
    req.sdiag_family = family;
    req.sdiag_protocol = protocol;
    req.idiag_states = (1 << TCP_ESTABLISHED);
    req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
    
    req.id.idiag_sport = htons(src_port);
    req.id.idiag_dport = htons(dst_port);
    req.id.idiag_src[0] = src_ip;
    req.id.idiag_dst[0] = dst_ip;
    
    if (send_diag_request_exact(&req, &response, &resp_len, &seq) == 0 && response) {
        struct nlmsghdr *nh;
        size_t remaining = resp_len;
        for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
            if (!is_valid_reply(nh, seq)) continue;
            if (nh->nlmsg_type == NLMSG_DONE) break;
            if (nh->nlmsg_type == NLMSG_ERROR) break;
            
            struct inet_diag_msg *diag = (struct inet_diag_msg*)NLMSG_DATA(nh);
            if (is_v4_match(diag, src_ip, src_port, dst_ip, dst_port)) {
                inode = diag->idiag_inode;
                break;
            }
        }
        ret = inode;
    }
    
    free(response);
    return ret;
}

/* ========== Connections ========== */

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
    uint32_t seq = 0;
    int ret = -1;
    
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
    
    for (int f = 0; f < num_families; f++) {
        memset(&req, 0, sizeof(req));
        req.sdiag_family = families[f];
        req.sdiag_protocol = (filter && filter->protocol > 0) ? filter->protocol : IPPROTO_TCP;
        req.idiag_states = (filter && filter->state_mask) ? filter->state_mask : (1 << TCP_ESTABLISHED);
        req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
        req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;
        
        if (send_diag_request_dump(&req, &response, &resp_len, &seq) == 0 && response) {
            struct nlmsghdr *nh;
            size_t remaining = resp_len;
            for (nh = (struct nlmsghdr*)response; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
                if (!is_valid_reply(nh, seq)) continue;
                if (nh->nlmsg_type == NLMSG_DONE) break;
                if (nh->nlmsg_type == NLMSG_ERROR) break;
                
                if (list_size >= list_capacity) {
                    list_capacity = list_capacity ? list_capacity * 2 : 64;
                    connection_info_t *new_list = realloc(list, sizeof(connection_info_t) * list_capacity);
                    if (!new_list) {
                        free(list);
                        free(response);
                        return -1;
                    }
                    list = new_list;
                }
                
                int parse_ret;
                if (families[f] == AF_INET) {
                    parse_ret = parse_diag_message_v4(nh, &list[list_size]);
                } else {
                    parse_ret = parse_diag_message_v6(nh, &list[list_size]);
                }
                
                if (parse_ret != 0) continue;
                
                if (filter_uid > 0 && list[list_size].uid != filter_uid) continue;
                if (filter) {
                    if (filter->src_port > 0 && list[list_size].src_port != filter->src_port) continue;
                    if (filter->dst_port > 0 && list[list_size].dst_port != filter->dst_port) continue;
                }
                
                list_size++;
            }
            free(response);
            response = NULL;
        }
    }
    
    *conns = list;
    *count = list_size;
    ret = 0;
    return ret;
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
