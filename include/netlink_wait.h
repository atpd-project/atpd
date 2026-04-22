#ifndef ATP_NETLINK_WAIT_H
#define ATP_NETLINK_WAIT_H

#include "reactor.h"
#include <stdint.h>

/**
 * Callback for interface wait completion
 * @param ifname    Interface name
 * @param success   1 if interface appeared within timeout, 0 on timeout
 * @param userdata  User data passed to netlink_wait_for_iface
 */
typedef void (*netlink_wait_cb)(const char *ifname, int success, void *userdata);

/**
 * Asynchronously wait for a network interface to appear
 * 
 * This function returns immediately and monitors the interface via:
 * - Netlink RTM_NEWLINK events (real-time notification)
 * - Periodic timer (fallback check every 1 second)
 * 
 * @param r          Reactor instance
 * @param ifname     Interface name to wait for
 * @param timeout_ms Timeout in milliseconds (0 = default 30s)
 * @param callback   Callback function when interface appears or timeout
 * @param userdata   User data passed to callback
 * @return 0 on success, -1 on error
 */
int netlink_wait_for_iface(reactor_t *r, const char *ifname, uint32_t timeout_ms,
                           netlink_wait_cb callback, void *userdata);

/**
 * Cleanup netlink wait resources
 */
void netlink_wait_cleanup(void);

#endif /* ATP_NETLINK_WAIT_H */
