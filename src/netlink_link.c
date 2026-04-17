#include "netlink_link.h"
#include "logger.h"
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define NL_BUF_SIZE 8192
#define NL_SEQ 12347

static int nl_send_link_request(int sock, int flags) {
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req;
    
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | flags;
    req.nlh.nlmsg_seq = NL_SEQ;
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

static int nl_recv_link_response(int sock, void **data, int *len) {
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

static void nl_parse_link_attrs(struct nl_link *link, struct rtattr *attrs, int len) {
    struct rtattr *rta;
    int rta_len = len;
    
    for (rta = attrs; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        switch (rta->rta_type) {
            case IFLA_IFNAME:
                strncpy(link->name, (char*)RTA_DATA(rta), IFNAMSIZ - 1);
                link->name[IFNAMSIZ - 1] = '\0';
                break;
            case IFLA_QDISC:
                strncpy(link->qdisc, (char*)RTA_DATA(rta), IFNAMSIZ - 1);
                link->qdisc[IFNAMSIZ - 1] = '\0';
                break;
            case IFLA_MTU:
                link->mtu = *((int*)RTA_DATA(rta));
                break;
            case IFLA_TXQLEN:
                link->tx_queue_len = *((int*)RTA_DATA(rta));
                break;
            case IFLA_ADDRESS: {
                int addr_len = RTA_PAYLOAD(rta);
                if (addr_len > 8) addr_len = 8;
                memcpy(link->address.data, RTA_DATA(rta), addr_len);
                link->address.len = addr_len;
                break;
            }
            case IFLA_BROADCAST: {
                int addr_len = RTA_PAYLOAD(rta);
                if (addr_len > 8) addr_len = 8;
                memcpy(link->broadcast.data, RTA_DATA(rta), addr_len);
                link->broadcast.len = addr_len;
                break;
            }
            case IFLA_CARRIER:
                link->carrier = *((uint8_t*)RTA_DATA(rta));
                break;
            case IFLA_CARRIER_CHANGES:
                link->carrier_changes = *((uint32_t*)RTA_DATA(rta));
                break;
        }
    }
}

int nl_link_list(struct nl_link **links, int *count) {
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
    
    if (nl_send_link_request(sock, NLM_F_DUMP) < 0) {
        LOG_ERROR("Failed to send netlink request: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    void *data;
    int data_len;
    if (nl_recv_link_response(sock, &data, &data_len) < 0) {
        LOG_ERROR("Failed to receive netlink response: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    close(sock);
    
    struct nl_link *link_list = NULL;
    int link_count = 0;
    struct nlmsghdr *nlh;
    
    for (nlh = (struct nlmsghdr*)data; NLMSG_OK(nlh, data_len); nlh = NLMSG_NEXT(nlh, data_len)) {
        if (nlh->nlmsg_type == NLMSG_DONE) break;
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            LOG_ERROR("Netlink error response");
            free(data);
            return -1;
        }
        
        if (nlh->nlmsg_type == RTM_NEWLINK) {
            struct ifinfomsg *ifi = (struct ifinfomsg*)NLMSG_DATA(nlh);
            int attr_len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
            
            struct nl_link *link = malloc(sizeof(struct nl_link));
            if (!link) continue;
            
            memset(link, 0, sizeof(struct nl_link));
            link->index = ifi->ifi_index;
            link->flags = ifi->ifi_flags;
            
            if (attr_len > 0) {
                struct rtattr *rta = (struct rtattr*)((char*)ifi + NLMSG_ALIGN(sizeof(*ifi)));
                nl_parse_link_attrs(link, rta, attr_len);
            }
            
            link_list = realloc(link_list, sizeof(struct nl_link) * (link_count + 1));
            if (!link_list) {
                free(link);
                break;
            }
            link_list[link_count++] = *link;
            free(link);
        }
    }
    
    free(data);
    *links = link_list;
    *count = link_count;
    return 0;
}

void nl_link_free(struct nl_link *links, int count) {
    if (!links) return;
    free(links);
}

int nl_link_get_by_name(const char *name, struct nl_link *link) {
    struct nl_link *links;
    int count;
    
    if (nl_link_list(&links, &count) < 0) {
        return -1;
    }
    
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(links[i].name, name) == 0) {
            memcpy(link, &links[i], sizeof(struct nl_link));
            found = 1;
            break;
        }
    }
    
    nl_link_free(links, count);
    return found ? 0 : -1;
}

int nl_link_get_by_index(int index, struct nl_link *link) {
    struct nl_link *links;
    int count;
    
    if (nl_link_list(&links, &count) < 0) {
        return -1;
    }
    
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (links[i].index == index) {
            memcpy(link, &links[i], sizeof(struct nl_link));
            found = 1;
            break;
        }
    }
    
    nl_link_free(links, count);
    return found ? 0 : -1;
}

int nl_link_get_index_by_name(const char *name) {
    struct nl_link link;
    if (nl_link_get_by_name(name, &link) < 0) {
        return -1;
    }
    return link.index;
}

const char* nl_link_get_name_by_index(int index) {
    static char name[IFNAMSIZ];
    struct nl_link link;
    
    if (nl_link_get_by_index(index, &link) < 0) {
        return NULL;
    }
    
    strncpy(name, link.name, IFNAMSIZ - 1);
    name[IFNAMSIZ - 1] = '\0';
    return name;
}

int nl_link_get_vpn_interface(char *iface, size_t size) {
    struct nl_link *links;
    int count;
    
    if (nl_link_list(&links, &count) < 0) {
        return -1;
    }
    
    int found = 0;
    for (int i = 0; i < count; i++) {
        /* Check for typical VPN interface naming patterns */
        if (strncmp(links[i].name, "tun", 3) == 0 ||
            strncmp(links[i].name, "wg", 2) == 0 ||
            strncmp(links[i].name, "ipsec", 5) == 0 ||
            strncmp(links[i].name, "vpn", 3) == 0) {
            if (links[i].flags & IFF_RUNNING) {
                strncpy(iface, links[i].name, size - 1);
                iface[size - 1] = '\0';
                found = 1;
                break;
            }
        }
    }
    
    nl_link_free(links, count);
    return found ? 0 : -1;
}
