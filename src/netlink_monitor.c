#include "netlink_monitor.h"
#include "logger.h"
#include "utils.h"
#include "routing.h"
#include "atp.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <net/if.h>
#include <arpa/inet.h>

#define NL_BUF_SIZE 65536
#define NL_MONITOR_TIMEOUT_MS 1000

static pthread_t g_monitor_thread;
static int g_monitor_running = 0;
static int g_monitor_sock = -1;
static nl_monitor_config_t g_monitor_config;
static pthread_mutex_t g_monitor_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Forward declarations */
static void* netlink_monitor_loop(void *arg);
static int is_vpn_interface(const char *iface);
static void handle_link_message(struct nlmsghdr *nlh);
static void handle_addr_message(struct nlmsghdr *nlh);
static void handle_route_message(struct nlmsghdr *nlh);

/* Helper: Check if interface is a VPN interface */
static int is_vpn_interface(const char *iface) {
    if (!iface || !iface[0]) return 0;
    return (strncmp(iface, "ipsec", 5) == 0 ||
            strncmp(iface, "tun", 3) == 0 ||
            strncmp(iface, "wg", 2) == 0 ||
            strncmp(iface, "vpn", 3) == 0);
}

/* Helper: Get interface name by index */
static int get_iface_name(int ifindex, char *ifname, size_t size) {
    if (if_indextoname(ifindex, ifname) != NULL) {
        return 0;
    }
    
    /* Fallback: query via sysfs */
    char path[256];
    FILE *fp;
    snprintf(path, sizeof(path), "/sys/class/net/if%d", ifindex);
    if ((fp = fopen(path, "r")) != NULL) {
        if (fgets(ifname, size, fp)) {
            char *newline = strchr(ifname, '\n');
            if (newline) *newline = '\0';
            fclose(fp);
            return 0;
        }
        fclose(fp);
    }
    
    return -1;
}

/* Handle RTM_NEWLINK / RTM_DELLINK messages */
static void handle_link_message(struct nlmsghdr *nlh) {
    struct ifinfomsg *ifi = (struct ifinfomsg*)NLMSG_DATA(nlh);
    char ifname[IFNAMSIZ];
    nl_event_type_t event;
    
    if (get_iface_name(ifi->ifi_index, ifname, sizeof(ifname)) != 0) {
        LOG_DEBUG("Failed to get interface name for index %d", ifi->ifi_index);
        return;
    }
    
    /* Filter by VPN only if configured */
    if (g_monitor_config.monitor_vpn_only && !is_vpn_interface(ifname)) {
        return;
    }
    
    if (nlh->nlmsg_type == RTM_NEWLINK) {
        int is_up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
        if (is_up) {
            event = NL_EVENT_LINK_UP;
            LOG_DEBUG("Netlink: Interface %s is UP", ifname);
        } else {
            event = NL_EVENT_LINK_DOWN;
            LOG_DEBUG("Netlink: Interface %s is DOWN", ifname);
        }
    } else if (nlh->nlmsg_type == RTM_DELLINK) {
        event = NL_EVENT_LINK_REMOVED;
        LOG_DEBUG("Netlink: Interface %s removed", ifname);
    } else {
        return;
    }
    
    if (g_monitor_config.callback) {
        g_monitor_config.callback(event, ifname, g_monitor_config.userdata);
    }
}

/* Handle RTM_NEWADDR / RTM_DELADDR messages */
static void handle_addr_message(struct nlmsghdr *nlh) {
    struct ifaddrmsg *ifa = (struct ifaddrmsg*)NLMSG_DATA(nlh);
    char ifname[IFNAMSIZ];
    nl_event_type_t event;
    char addr_str[64] = {0};
    struct rtattr *rta;
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*ifa));
    
    if (get_iface_name(ifa->ifa_index, ifname, sizeof(ifname)) != 0) {
        return;
    }
    
    /* Filter by VPN only if configured */
    if (g_monitor_config.monitor_vpn_only && !is_vpn_interface(ifname)) {
        return;
    }
    
    /* Extract address for logging */
    for (rta = IFA_RTA(ifa); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFA_ADDRESS) {
            if (ifa->ifa_family == AF_INET) {
                inet_ntop(AF_INET, RTA_DATA(rta), addr_str, sizeof(addr_str));
            } else if (ifa->ifa_family == AF_INET6) {
                inet_ntop(AF_INET6, RTA_DATA(rta), addr_str, sizeof(addr_str));
            }
            break;
        }
    }
    
    if (nlh->nlmsg_type == RTM_NEWADDR) {
        event = NL_EVENT_ADDR_ADDED;
        LOG_DEBUG("Netlink: Address %s added to %s", addr_str, ifname);
    } else {
        event = NL_EVENT_ADDR_REMOVED;
        LOG_DEBUG("Netlink: Address %s removed from %s", addr_str, ifname);
    }
    
    if (g_monitor_config.callback) {
        g_monitor_config.callback(event, ifname, g_monitor_config.userdata);
    }
}

/* Handle RTM_NEWROUTE / RTM_DELROUTE messages */
static void handle_route_message(struct nlmsghdr *nlh) {
    struct rtmsg *rtm = (struct rtmsg*)NLMSG_DATA(nlh);
    nl_event_type_t event;
    char dst_str[64] = {0};
    char gw_str[64] = {0};
    struct rtattr *rta;
    int rta_len = NLMSG_PAYLOAD(nlh, sizeof(*rtm));
    
    /* Only care about main table routes */
    if (rtm->rtm_table != RT_TABLE_MAIN && rtm->rtm_table != RT_TABLE_UNSPEC) {
        return;
    }
    
    /* Parse route attributes */
    for (rta = RTM_RTA(rtm); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == RTA_DST) {
            if (rtm->rtm_family == AF_INET) {
                inet_ntop(AF_INET, RTA_DATA(rta), dst_str, sizeof(dst_str));
            }
        } else if (rta->rta_type == RTA_GATEWAY) {
            if (rtm->rtm_family == AF_INET) {
                inet_ntop(AF_INET, RTA_DATA(rta), gw_str, sizeof(gw_str));
            }
        }
    }
    
    if (nlh->nlmsg_type == RTM_NEWROUTE) {
        event = NL_EVENT_ROUTE_ADDED;
        LOG_DEBUG("Netlink: Route %s via %s added", dst_str, gw_str);
    } else {
        event = NL_EVENT_ROUTE_REMOVED;
        LOG_DEBUG("Netlink: Route %s via %s removed", dst_str, gw_str);
    }
    
    if (g_monitor_config.callback) {
        g_monitor_config.callback(event, NULL, g_monitor_config.userdata);
    }
}

/* Netlink receiver thread main loop */
static void* netlink_monitor_loop(void *arg) {
    struct sockaddr_nl sa;
    char buf[NL_BUF_SIZE];
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg;
    int group_mask = 0;
    
    (void)arg;
    
    LOG_INFO("Netlink monitor thread started");
    
    /* Create netlink socket */
    g_monitor_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (g_monitor_sock < 0) {
        LOG_ERROR("Failed to create netlink socket: %s", strerror(errno));
        return NULL;
    }
    
    /* Build group mask based on configuration */
    if (g_monitor_config.monitor_links) {
        group_mask |= RTMGRP_LINK;
    }
    if (g_monitor_config.monitor_addrs) {
        group_mask |= RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    }
    if (g_monitor_config.monitor_routes) {
        group_mask |= RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    }
    
    /* Bind socket */
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = group_mask;
    sa.nl_pid = getpid();
    
    if (bind(g_monitor_sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        LOG_ERROR("Failed to bind netlink socket: %s", strerror(errno));
        close(g_monitor_sock);
        g_monitor_sock = -1;
        return NULL;
    }
    
    /* Setup message header */
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    LOG_INFO("Netlink monitor listening for events (groups=0x%x)", group_mask);
    
    /* Main receive loop */
    while (g_monitor_running) {
        ssize_t len;
        
        /* Set socket timeout to check g_monitor_running periodically */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(g_monitor_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        len = recvmsg(g_monitor_sock, &msg, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  /* Timeout, check running flag */
            }
            LOG_DEBUG("recvmsg error: %s", strerror(errno));
            continue;
        }
        
        /* Parse netlink messages */
        struct nlmsghdr *nh;
        for (nh = (struct nlmsghdr*)buf; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE) {
                break;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nh);
                LOG_DEBUG("Netlink error: %d", err->error);
                continue;
            }
            
            if (g_monitor_config.monitor_links &&
                (nh->nlmsg_type == RTM_NEWLINK || nh->nlmsg_type == RTM_DELLINK)) {
                handle_link_message(nh);
            }
            
            if (g_monitor_config.monitor_addrs &&
                (nh->nlmsg_type == RTM_NEWADDR || nh->nlmsg_type == RTM_DELADDR)) {
                handle_addr_message(nh);
            }
            
            if (g_monitor_config.monitor_routes &&
                (nh->nlmsg_type == RTM_NEWROUTE || nh->nlmsg_type == RTM_DELROUTE)) {
                handle_route_message(nh);
            }
        }
    }
    
    LOG_INFO("Netlink monitor thread stopping");
    if (g_monitor_sock >= 0) {
        close(g_monitor_sock);
        g_monitor_sock = -1;
    }
    
    return NULL;
}

/* Start the netlink monitor thread */
int netlink_monitor_start(nl_monitor_config_t *config) {
    pthread_mutex_lock(&g_monitor_mutex);
    
    if (g_monitor_running) {
        LOG_WARN("Netlink monitor already running");
        pthread_mutex_unlock(&g_monitor_mutex);
        return 0;
    }
    
    /* Copy configuration */
    memcpy(&g_monitor_config, config, sizeof(nl_monitor_config_t));
    
    /* Set default monitor flags if none specified */
    if (!g_monitor_config.monitor_links &&
        !g_monitor_config.monitor_addrs &&
        !g_monitor_config.monitor_routes) {
        g_monitor_config.monitor_links = 1;
        g_monitor_config.monitor_addrs = 1;
        LOG_DEBUG("Netlink monitor: defaulting to link and address monitoring");
    }
    
    g_monitor_running = 1;
    
    if (pthread_create(&g_monitor_thread, NULL, netlink_monitor_loop, NULL) != 0) {
        LOG_ERROR("Failed to create netlink monitor thread");
        g_monitor_running = 0;
        pthread_mutex_unlock(&g_monitor_mutex);
        return -1;
    }
    
    pthread_mutex_unlock(&g_monitor_mutex);
    LOG_INFO("Netlink monitor started");
    return 0;
}

/* Stop the netlink monitor thread */
void netlink_monitor_stop(void) {
    pthread_mutex_lock(&g_monitor_mutex);
    
    if (!g_monitor_running) {
        pthread_mutex_unlock(&g_monitor_mutex);
        return;
    }
    
    g_monitor_running = 0;
    pthread_mutex_unlock(&g_monitor_mutex);
    
    /* Wake up the socket by sending a signal */
    if (g_monitor_sock >= 0) {
        shutdown(g_monitor_sock, SHUT_RD);
    }
    
    pthread_join(g_monitor_thread, NULL);
    LOG_INFO("Netlink monitor stopped");
}

/* Check if monitor is running */
int netlink_monitor_is_running(void) {
    return g_monitor_running;
}

/* Default callback that integrates with existing routing module */
void netlink_default_callback(nl_event_type_t event, const char *iface, void *userdata) {
    atp_config_t *cfg = (atp_config_t*)userdata;
    
    if (!cfg || !iface) return;
    
    switch (event) {
        case NL_EVENT_LINK_UP:
            if (is_vpn_interface(iface)) {
                LOG_INFO("VPN interface %s detected, applying policies", iface);
                routing_add_vpn_policy(cfg, iface);
                routing_add_mss_clamp(cfg, iface);
            }
            break;
            
        case NL_EVENT_LINK_DOWN:
            if (is_vpn_interface(iface)) {
                LOG_INFO("VPN interface %s down, removing policies", iface);
                routing_remove_vpn_policy(cfg, iface);
                routing_remove_mss_clamp(cfg, iface);
            }
            break;
            
        case NL_EVENT_VPN_CONNECTED:
            LOG_INFO("VPN connection established");
            break;
            
        case NL_EVENT_VPN_DISCONNECTED:
            LOG_INFO("VPN connection terminated");
            break;
            
        default:
            break;
    }
}
