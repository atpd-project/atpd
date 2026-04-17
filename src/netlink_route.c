#include "netlink_route.h"
#include "logger.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define NL_BUF_SIZE 8192
#define NL_SEQ 12346

static int nl_send_route_request(int sock, int family, int table, int flags) {
    struct {
        struct nlmsghdr nlh;
        struct rtmsg rtm;
    } req;
    
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type = RTM_GETROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | flags;
    req.nlh.nlmsg_seq = NL_SEQ;
    req.nlh.nlmsg_pid = getpid();
    req.rtm.rtm_family = family;
    req.rtm.rtm_table = table;
    
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

static int nl_recv_route_response(int sock, void **data, int *len) {
    char buf[NL_BUF_SIZE];
    struct sockaddr_nl addr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };
    
    ssize_t recv_len = recvmsg(sock, &msg, 0);
    if (recv_len < 0) return -1;
    
    *data = malloc(recv_len);
    if (!*data) return -1;
    memcpy(*data, buf, recv_len);
    *len = recv_len;
    return 0;
}

static void nl_parse_route_attrs(struct nl_route *route, struct rtattr *attrs, int len) {
    struct rtattr *rta;
    int rta_len = len;
    
    for (rta = attrs; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case RTA_DST:
                if (route->family == AF_INET && RTA_PAYLOAD(rta) >= 4) {
                    memcpy(&route->dst, RTA_DATA(rta), 4);
                } else if (route->family == AF_INET6 && RTA_PAYLOAD(rta) >= 16) {
                    memcpy(&route->dst, RTA_DATA(rta), 16);
                }
                break;
            case RTA_SRC:
                if (route->family == AF_INET && RTA_PAYLOAD(rta) >= 4) {
                    memcpy(&route->src, RTA_DATA(rta), 4);
                } else if (route->family == AF_INET6 && RTA_PAYLOAD(rta) >= 16) {
                    memcpy(&route->src, RTA_DATA(rta), 16);
                }
                break;
            case RTA_GATEWAY:
                if (route->family == AF_INET && RTA_PAYLOAD(rta) >= 4) {
                    memcpy(&route->gw, RTA_DATA(rta), 4);
                } else if (route->family == AF_INET6 && RTA_PAYLOAD(rta) >= 16) {
                    memcpy(&route->gw, RTA_DATA(rta), 16);
                }
                break;
            case RTA_OIF:
                route->link_index = *((int*)RTA_DATA(rta));
                break;
            case RTA_PRIORITY:
                route->priority = *((uint32_t*)RTA_DATA(rta));
                break;
            case RTA_TABLE:
                route->table = *((uint32_t*)RTA_DATA(rta));
                break;
            case RTA_MARK:
                route->mark = *((uint32_t*)RTA_DATA(rta));
                break;
        }
    }
}

static const char* nl_get_iface_name(int link_index) {
    static char ifname[IFNAMSIZ];
    if_indextoname(link_index, ifname);
    return ifname;
}

int nl_route_list_by_table(struct nl_route **routes, int *count, int table) {
    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0) {
        LOG_ERROR("Failed to create netlink socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind netlink socket: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    /* Request routes for both IPv4 and IPv6 */
    if (nl_send_route_request(sock, AF_INET, table, NLM_F_DUMP) < 0 &&
        nl_send_route_request(sock, AF_INET6, table, NLM_F_DUMP) < 0) {
        LOG_ERROR("Failed to send netlink request: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    void *data;
    int data_len;
    if (nl_recv_route_response(sock, &data, &data_len) < 0) {
        LOG_ERROR("Failed to receive netlink response: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    close(sock);
    
    struct nl_route *route_list = NULL;
    int route_count = 0;
    struct nlmsghdr *nlh;
    
    for (nlh = (struct nlmsghdr*)data; NLMSG_OK(nlh, data_len); nlh = NLMSG_NEXT(nlh, data_len)) {
        if (nlh->nlmsg_type == NLMSG_DONE) break;
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            LOG_ERROR("Netlink error response");
            free(data);
            return -1;
        }
        
        if (nlh->nlmsg_type == RTM_NEWROUTE) {
            struct rtmsg *rtm = (struct rtmsg*)NLMSG_DATA(nlh);
            int attr_len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm));
            
            struct nl_route *route = malloc(sizeof(struct nl_route));
            if (!route) continue;
            
            memset(route, 0, sizeof(struct nl_route));
            route->family = rtm->rtm_family;
            route->dst_len = rtm->rtm_dst_len;
            route->src_len = rtm->rtm_src_len;
            route->protocol = rtm->rtm_protocol;
            route->scope = rtm->rtm_scope;
            route->type = rtm->rtm_type;
            route->table = rtm->rtm_table;
            route->flags = rtm->rtm_flags;
            
            if (attr_len > 0) {
                struct rtattr *rta = (struct rtattr*)((char*)rtm + NLMSG_ALIGN(sizeof(*rtm)));
                nl_parse_route_attrs(route, rta, attr_len);
            }
            
            /* Get interface name from link index */
            if (route->link_index > 0) {
                strncpy(route->iface, nl_get_iface_name(route->link_index), IFNAMSIZ - 1);
                route->iface[IFNAMSIZ - 1] = '\0';
            }
            
            route_list = realloc(route_list, sizeof(struct nl_route) * (route_count + 1));
            if (!route_list) {
                free(route);
                break;
            }
            route_list[route_count++] = *route;
            free(route);
        }
    }
    
    free(data);
    *routes = route_list;
    *count = route_count;
    return 0;
}

int nl_route_list(struct nl_route **routes, int *count) {
    return nl_route_list_by_table(routes, count, RT_TABLE_MAIN);
}

void nl_route_free(struct nl_route *routes, int count) {
    if (!routes) return;
    free(routes);
}

int nl_route_get_default_table(void) {
    struct nl_route *routes;
    int count;
    
    if (nl_route_list(&routes, &count) < 0) {
        return NL_ROUTE_TABLE_MAIN;
    }
    
    int default_table = NL_ROUTE_TABLE_MAIN;
    for (int i = 0; i < count; i++) {
        /* Default route is dst_len == 0 */
        if (routes[i].dst_len == 0 && routes[i].family == AF_INET) {
            default_table = routes[i].table;
            break;
        }
    }
    
    nl_route_free(routes, count);
    return default_table;
}

int nl_route_get_table_by_mark(uint32_t mark) {
    struct nl_route *routes;
    int count;
    
    if (nl_route_list(&routes, &count) < 0) {
        return NL_ROUTE_TABLE_MAIN;
    }
    
    int table = NL_ROUTE_TABLE_MAIN;
    for (int i = 0; i < count; i++) {
        if (routes[i].mark == mark) {
            table = routes[i].table;
            break;
        }
    }
    
    nl_route_free(routes, count);
    return table;
}
