#include "iface_monitor.h"
#include "logger.h"
#include "netlink.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <net/if.h>

#define NL_BUF_SIZE 8192
#define EPOLL_TIMEOUT_MS 1000

/* Helper function to get interface name by index */
static int get_ifname_by_index(int ifindex, char *ifname, size_t size) {
    char cmd[MAX_CMD_LEN];
    char output[IFNAMSIZ + 1];
    
    snprintf(cmd, sizeof(cmd), 
             "ip link show %d 2>/dev/null | head -1 | awk '{print $2}' | tr -d ':'",
             ifindex);
    
    if (exec_cmd(cmd, output, sizeof(output), 3) == 0 && output[0] != '\0') {
        strncpy(ifname, output, size - 1);
        ifname[size - 1] = '\0';
        return 0;
    }
    
    /* Fallback: use if_indextoname if available */
    if (if_indextoname(ifindex, ifname) != NULL) {
        return 0;
    }
    
    return -1;
}

static int nl_send_link_dump(int sock) {
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req;
    
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.nlh.nlmsg_pid = getpid();
    req.ifi.ifi_family = AF_UNSPEC;
    
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

static int nl_send_addr_dump(int sock, int family) {
    struct {
        struct nlmsghdr nlh;
        struct rtgenmsg g;
    } req;
    
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    req.nlh.nlmsg_type = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.nlh.nlmsg_pid = getpid();
    req.g.rtgen_family = family;
    
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

static void handle_link_message(struct nlmsghdr *nlh, iface_monitor_t *monitor) {
    struct ifinfomsg *ifi = (struct ifinfomsg*)NLMSG_DATA(nlh);
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
    
    if (nlh->nlmsg_type == RTM_NEWLINK) {
        if (ifi->ifi_flags & IFF_RUNNING) {
            LOG_DEBUG("Interface %s is UP", ifname);
            if (monitor->callback) {
                monitor->callback(ifname, IFACE_EVENT_UP, monitor->userdata);
            }
        } else {
            LOG_DEBUG("Interface %s is DOWN", ifname);
            if (monitor->callback) {
                monitor->callback(ifname, IFACE_EVENT_DOWN, monitor->userdata);
            }
        }
    }
}

static void handle_addr_message(struct nlmsghdr *nlh, iface_monitor_t *monitor) {
    struct ifaddrmsg *ifa = (struct ifaddrmsg*)NLMSG_DATA(nlh);
    struct rtattr *rta;
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifa));
    char ifname[IFNAMSIZ] = {0};
    char addr_str[64] = {0};
    
    /* Get real interface name by index */
    if (get_ifname_by_index(ifa->ifa_index, ifname, sizeof(ifname)) != 0) {
        /* Fallback: use generic name if lookup fails */
        snprintf(ifname, sizeof(ifname), "if%d", ifa->ifa_index);
    }
    
    for (rta = IFA_RTA(ifa); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFA_ADDRESS) {
            if (ifa->ifa_family == AF_INET) {
                inet_ntop(AF_INET, RTA_DATA(rta), addr_str, sizeof(addr_str));
            } else if (ifa->ifa_family == AF_INET6) {
                inet_ntop(AF_INET6, RTA_DATA(rta), addr_str, sizeof(addr_str));
            }
        }
    }
    
    if (addr_str[0]) {
        if (nlh->nlmsg_type == RTM_NEWADDR) {
            LOG_DEBUG("Address %s added to %s", addr_str, ifname);
            if (monitor->callback) {
                monitor->callback(ifname, IFACE_EVENT_ADDR_ADDED, monitor->userdata);
            }
        } else if (nlh->nlmsg_type == RTM_DELADDR) {
            LOG_DEBUG("Address %s removed from %s", addr_str, ifname);
            if (monitor->callback) {
                monitor->callback(ifname, IFACE_EVENT_ADDR_REMOVED, monitor->userdata);
            }
        }
    }
}

static int is_vpn_interface(const char *ifname) {
    return (strncmp(ifname, "ipsec", 5) == 0 ||
            strncmp(ifname, "tun", 3) == 0 ||
            strncmp(ifname, "wg", 2) == 0 ||
            strncmp(ifname, "vpn", 3) == 0);
}

static void check_vpn_status(iface_monitor_t *monitor) {
    int vpn_enabled = nl_vpn_detect();
    
    if (vpn_enabled != monitor->vpn_enabled) {
        monitor->vpn_enabled = vpn_enabled;
        if (monitor->callback) {
            iface_event_t event = vpn_enabled ? IFACE_EVENT_VPN_CONNECTED : IFACE_EVENT_VPN_DISCONNECTED;
            monitor->callback(NULL, event, monitor->userdata);
        }
    }
    
    /* Also check for VPN interface by name */
    char vpn_iface[IFNAMSIZ];
    if (nl_link_get_vpn_interface(vpn_iface, sizeof(vpn_iface)) == 0) {
        if (strcmp(vpn_iface, monitor->current_vpn_iface) != 0) {
            if (monitor->current_vpn_iface[0] != '\0') {
                LOG_INFO("VPN interface changed: %s -> %s", 
                         monitor->current_vpn_iface, vpn_iface);
            }
            strncpy(monitor->current_vpn_iface, vpn_iface, IFNAMSIZ - 1);
            if (monitor->callback) {
                monitor->callback(vpn_iface, IFACE_EVENT_ADDED, monitor->userdata);
            }
        }
    }
}

int iface_monitor_init(iface_monitor_t *monitor, iface_callback_t callback, void *userdata) {
    memset(monitor, 0, sizeof(iface_monitor_t));
    
    monitor->sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (monitor->sock_fd < 0) {
        LOG_ERROR("Failed to create netlink socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    
    if (bind(monitor->sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind netlink socket: %s", strerror(errno));
        close(monitor->sock_fd);
        return -1;
    }
    
    monitor->callback = callback;
    monitor->userdata = userdata;
    monitor->running = 0;
    monitor->vpn_enabled = 0;
    monitor->current_vpn_iface[0] = '\0';
    
    LOG_DEBUG("Interface monitor initialized");
    return 0;
}

int iface_monitor_start(iface_monitor_t *monitor) {
    if (!monitor || monitor->sock_fd < 0) {
        return -1;
    }
    
    monitor->running = 1;
    
    /* Initial dump of current state */
    nl_send_link_dump(monitor->sock_fd);
    nl_send_addr_dump(monitor->sock_fd, AF_INET);
    nl_send_addr_dump(monitor->sock_fd, AF_INET6);
    
    check_vpn_status(monitor);
    
    LOG_DEBUG("Interface monitor started");
    return 0;
}

int iface_monitor_poll(iface_monitor_t *monitor, int timeout_ms) {
    if (!monitor || !monitor->running || monitor->sock_fd < 0) {
        return -1;
    }
    
    struct epoll_event ev;
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        return -1;
    }
    
    ev.events = EPOLLIN;
    ev.data.fd = monitor->sock_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, monitor->sock_fd, &ev);
    
    struct epoll_event events[10];
    int nfds = epoll_wait(epoll_fd, events, 10, timeout_ms);
    
    if (nfds > 0) {
        char buf[NL_BUF_SIZE];
        struct sockaddr_nl addr;
        struct iovec iov = { buf, sizeof(buf) };
        struct msghdr msg = {
            .msg_name = &addr,
            .msg_namelen = sizeof(addr),
            .msg_iov = &iov,
            .msg_iovlen = 1
        };
        
        ssize_t len = recvmsg(monitor->sock_fd, &msg, 0);
        if (len > 0) {
            struct nlmsghdr *nlh;
            for (nlh = (struct nlmsghdr*)buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
                if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                    handle_link_message(nlh, monitor);
                } else if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
                    handle_addr_message(nlh, monitor);
                }
            }
        }
    }
    
    close(epoll_fd);
    
    /* Periodically check VPN status */
    static int check_counter = 0;
    if (++check_counter >= 10) {
        check_counter = 0;
        check_vpn_status(monitor);
    }
    
    return nfds;
}

int iface_monitor_stop(iface_monitor_t *monitor) {
    if (monitor) {
        monitor->running = 0;
    }
    LOG_DEBUG("Interface monitor stopped");
    return 0;
}

void iface_monitor_cleanup(iface_monitor_t *monitor) {
    if (monitor && monitor->sock_fd >= 0) {
        close(monitor->sock_fd);
        monitor->sock_fd = -1;
    }
    LOG_DEBUG("Interface monitor cleaned up");
}
