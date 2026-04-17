#ifndef ATP_MAC_FILTER_H
#define ATP_MAC_FILTER_H

#include "atp.h"
#include <net/if.h>
#include <net/ethernet.h>

typedef struct {
    uint8_t addr[ETH_ALEN];
    char addr_str[18];
} mac_addr_t;

int mac_filter_init(atp_config_t *cfg);
int mac_filter_setup(atp_config_t *cfg);
int mac_filter_cleanup(atp_config_t *cfg);
int mac_filter_parse_mac(const char *mac_str, uint8_t *mac_bytes);
void mac_filter_format_mac(const uint8_t *mac_bytes, char *buf, size_t size);
int mac_filter_parse_list(const char *macs_list, mac_addr_t **macs, int *count);
void mac_filter_free_list(mac_addr_t *macs);

#endif
