
#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define NL_BUF_SIZE 8192
#define EPOLL_TIMEOUT_MS 1000
#define IP_CMD "/system/bin/ip"

//static int nl_socket_fd = -1;
//static int epoll_fd = -1;
static volatile int monitoring = 0;

static int netlink_send_request(int sock, int type, int flags) {
    struct {
        struct nlmsghdr nlh;
        struct rtgenmsg g;
    } req;
    
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    req.nlh.nlmsg_type = type;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | flags;
    req.nlh.nlmsg_seq = 1;
    req.nlh.nlmsg_pid = getpid();
    req.g.rtgen_family = AF_UNSPEC;
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    struct iovec iov = { &req, req.nlh.nlmsg_len };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    return sendmsg(sock, &msg, 0);
}

int netlink_init(netlink_ctx_t *ctx) {
    memset(ctx, 0, sizeof(netlink_ctx_t));
    
    ctx->sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (ctx->sock_fd < 0) {
        LOG_ERROR("Failed to create netlink socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    
    if (bind(ctx->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind netlink socket: %s", strerror(errno));
        close(ctx->sock_fd);
        return -1;
    }
    
    ctx->epoll_fd = epoll_create1(0);
    if (ctx->epoll_fd < 0) {
        LOG_ERROR("Failed to create epoll fd: %s", strerror(errno));
        close(ctx->sock_fd);
        return -1;
    }
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ctx->sock_fd;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->sock_fd, &ev) < 0) {
        LOG_ERROR("Failed to add netlink socket to epoll: %s", strerror(errno));
        close(ctx->epoll_fd);
        close(ctx->sock_fd);
        return -1;
    }
    
    ctx->running = 1;
    LOG_DEBUG("Netlink initialized");
    return 0;
}

void netlink_cleanup(netlink_ctx_t *ctx) {
    if (ctx) {
        ctx->running = 0;
        if (ctx->epoll_fd >= 0) close(ctx->epoll_fd);
        if (ctx->sock_fd >= 0) close(ctx->sock_fd);
        ctx->epoll_fd = -1;
        ctx->sock_fd = -1;
    }
    LOG_DEBUG("Netlink cleaned up");
}

static void parse_link_message(struct nlmsghdr *nlh, netlink_callback_t callback, void *userdata) {
    struct ifinfomsg *ifi = NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifi));
    char ifname[IFNAMSIZ] = {0};
    
    for (rta = IFLA_RTA(ifi); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFLA_IFNAME) {
            strncpy(ifname, RTA_DATA(rta), IFNAMSIZ - 1);
            break;
        }
    }
    
    if (ifname[0] == '\0') return;
    
    int added = (nlh->nlmsg_type == RTM_NEWLINK);
    if (callback) {
        callback(ifname, added, ifi->ifi_index, userdata);
    }
}

static void parse_addr_message(struct nlmsghdr *nlh, netlink_callback_t callback, void *userdata) {
    struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifa));
    
    if (nlh->nlmsg_type != RTM_NEWADDR && nlh->nlmsg_type != RTM_DELADDR) return;
    
    char ifname[IFNAMSIZ] = {0};
    char addr_str[INET6_ADDRSTRLEN] = {0};
    
    for (rta = IFA_RTA(ifa); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFA_ADDRESS) {
            if (ifa->ifa_family == AF_INET) {
                inet_ntop(AF_INET, RTA_DATA(rta), addr_str, sizeof(addr_str));
            } else if (ifa->ifa_family == AF_INET6) {
                inet_ntop(AF_INET6, RTA_DATA(rta), addr_str, sizeof(addr_str));
            }
        }
    }
    
    if (ifname[0] == '\0') {
        snprintf(ifname, sizeof(ifname), "if%d", ifa->ifa_index);
    }
    
    int added = (nlh->nlmsg_type == RTM_NEWADDR);
    if (callback && addr_str[0]) {
        LOG_DEBUG("Addr %s on %s", added ? "added" : "removed", ifname);
        callback(ifname, added, ifa->ifa_index, userdata);
    }
}

int netlink_monitor_start(netlink_ctx_t *ctx, netlink_callback_t callback, void *userdata) {
    if (!ctx || ctx->sock_fd < 0) {
        LOG_ERROR("Netlink not initialized");
        return -1;
    }
    
    ctx->callback = callback;
    ctx->callback_data = userdata;
    ctx->running = 1;
    monitoring = 1;
    
    netlink_send_request(ctx->sock_fd, RTM_GETLINK, NLM_F_DUMP);
    netlink_send_request(ctx->sock_fd, RTM_GETADDR, NLM_F_DUMP);
    
    LOG_DEBUG("Netlink monitoring started");
    return 0;
}

int netlink_monitor_stop(netlink_ctx_t *ctx) {
    if (ctx) {
        ctx->running = 0;
        monitoring = 0;
    }
    LOG_DEBUG("Netlink monitoring stopped");
    return 0;
}

int netlink_monitor_poll(netlink_ctx_t *ctx, int timeout_ms) {
    if (!ctx || !ctx->running || ctx->sock_fd < 0) return -1;
    
    struct epoll_event events[10];
    int nfds = epoll_wait(ctx->epoll_fd, events, 10, timeout_ms);
    
    if (nfds < 0) {
        if (errno == EINTR) return 0;
        LOG_ERROR("epoll_wait failed: %s", strerror(errno));
        return -1;
    }
    
    for (int i = 0; i < nfds; i++) {
        if (events[i].data.fd == ctx->sock_fd) {
            char buf[NL_BUF_SIZE];
            struct sockaddr_nl addr;
            struct iovec iov = { buf, sizeof(buf) };
            struct msghdr msg = {
                .msg_name = &addr,
                .msg_namelen = sizeof(addr),
                .msg_iov = &iov,
                .msg_iovlen = 1
            };
            
            ssize_t len = recvmsg(ctx->sock_fd, &msg, 0);
            if (len < 0) {
                LOG_ERROR("recvmsg failed: %s", strerror(errno));
                continue;
            }
            
            struct nlmsghdr *nlh;
            for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
                if (nlh->nlmsg_type == NLMSG_DONE) break;
                if (nlh->nlmsg_type == NLMSG_ERROR) {
                    LOG_WARN("Netlink error received");
                    continue;
                }
                
                if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                    parse_link_message(nlh, ctx->callback, ctx->callback_data);
                } else if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
                    parse_addr_message(nlh, ctx->callback, ctx->callback_data);
                }
            }
        }
    }
    
    return 0;
}

int netlink_get_active_vpn(netlink_ctx_t *ctx, char *iface, size_t size) {
    (void)ctx;
    
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "%s -4 addr | grep -oE 'ipsec[0-9]+' | head -1", IP_CMD);
    
    char output[IFNAMSIZ];
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        strncpy(iface, output, size - 1);
        return 0;
    }
    
    iface[0] = '\0';
    return -1;
}

int netlink_wait_for_iface(const char *iface, int timeout_sec) {
    char cmd[MAX_CMD_LEN]
    char output[64];
    (void)output;
    int waited = 0;
    
    while (waited < timeout_sec) {
        snprintf(cmd, sizeof(cmd), 
                 "%s -4 addr show dev %s 2>/dev/null | grep -q 'inet '", IP_CMD, iface);
        if (exec_cmd_simple(cmd, 2) == 0) {
            LOG_DEBUG("Interface %s is ready after %d seconds", iface, waited);
            return 0;
        }
        sleep(1);
        waited++;
    }
    
    LOG_WARN("Interface %s not ready after %d seconds", iface, timeout_sec);
    return -1;
}

int netlink_get_iface_info(const char *iface, netlink_iface_info_t *info) {
    memset(info, 0, sizeof(netlink_iface_info_t));
    strncpy(info->iface, iface, sizeof(info->iface) - 1);
    
    char cmd[MAX_CMD_LEN];
    char output[256];
    
    snprintf(cmd, sizeof(cmd), 
             "%s -4 addr show dev %s 2>/dev/null | grep 'inet ' | head -1 | awk '{print $2}'",
             IP_CMD, iface);
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        char *slash = strchr(output, '/');
        if (slash) {
            *slash = '\0';
            info->ipv4_prefix = atoi(slash + 1);
        }
        strncpy(info->ipv4_addr, output, sizeof(info->ipv4_addr) - 1);
        info->has_ipv4 = 1;
    }
    
    snprintf(cmd, sizeof(cmd), 
             "%s -6 addr show dev %s 2>/dev/null | grep 'inet6 ' | grep -v 'fe80' | head -1 | awk '{print $2}'",
             IP_CMD, iface);
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        char *slash = strchr(output, '/');
        if (slash) {
            *slash = '\0';
            info->ipv6_prefix = atoi(slash + 1);
        }
        strncpy(info->ipv6_addr, output, sizeof(info->ipv6_addr) - 1);
        info->has_ipv6 = 1;
    }
    
    snprintf(cmd, sizeof(cmd), "cat /sys/class/net/%s/flags 2>/dev/null", iface);
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        info->flags = strtoul(output, NULL, 16);
    }
    
    snprintf(cmd, sizeof(cmd), "cat /sys/class/net/%s/mtu 2>/dev/null", iface);
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        info->mtu = atoi(output);
    }
    
    info->ifindex = if_nametoindex(iface);
    
    return 0;
}

int netlink_get_all_ifaces(netlink_iface_info_t *info_array, int max_count) {
    char cmd[MAX_CMD_LEN];
    char output[4096];
    int count = 0;
    
    snprintf(cmd, sizeof(cmd), "%s -brief link show | awk '{print $1}' | grep -v lo", IP_CMD);
    
    if (exec_cmd(cmd, output, sizeof(output), 5) != 0) {
        return 0;
    }
    
    char *line = strtok(output, " \n");
    while (line && count < max_count) {
        netlink_get_iface_info(line, &info_array[count]);
        count++;
        line = strtok(NULL, " \n");
    }
    
    return count;
}

int netlink_check_rule_exists(int table_id, int mark, const char *iface) {
    char cmd[MAX_CMD_LEN];
    char output[256];
    
    snprintf(cmd, sizeof(cmd), 
             "%s rule show | grep -q 'fwmark 0x%x.*lookup %d'", IP_CMD, mark, table_id);
    
    if (exec_cmd(cmd, output, sizeof(output), 5) != 0) {
        return 0;
    }
    
    if (iface && iface[0]) {
        snprintf(cmd, sizeof(cmd), 
                 "%s rule show | grep -q 'iif ap0.*lookup %s'", IP_CMD, iface);
        return exec_cmd(cmd, output, sizeof(output), 5) == 0;
    }
    
    return 1;
}

int netlink_get_ipv4_snapshot(char *output, size_t size) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "%s -4 addr show | grep 'inet ' | awk '{print $NF\":\"$2}' | grep -v lo: | tr '\\n' ' '",
             IP_CMD);
    return exec_cmd(cmd, output, size, 5);
}

int netlink_compare_ipv4_snapshot(const char *before, const char *after, char *diff, size_t size) {
    if (!before || !after) return -1;
    
    if (strcmp(before, after) == 0) {
        snprintf(diff, size, "Network Stable: [%s]", after);
        return 0;
    }
    
    snprintf(diff, size, "Network Shift: [%s] -> [%s]", before, after);
    return 1;
}
