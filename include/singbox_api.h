#ifndef ATP_SINGBOX_API_H
#define ATP_SINGBOX_API_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_PROXY_ITEMS 1024
#define MAX_RULE_ITEMS 1024
#define MAX_ALL_ITEMS 256

typedef struct {
    char *name;
    char *type;
    int delay;
    bool udp;
    char *now;
    char **all;
    int all_count;
    uint64_t updated_at;
} proxy_info_t;

typedef struct {
    proxy_info_t *proxies;
    int count;
} proxy_list_t;

typedef struct {
    char *type;
    char *payload;
    char *proxy;
    char *uuid;
} rule_info_t;

typedef struct {
    rule_info_t *rules;
    int count;
} rule_list_t;

typedef struct {
    uint64_t upload_total;
    uint64_t download_total;
    int connection_count;
    uint64_t timestamp;
} traffic_snapshot_t;

int singbox_parse_proxies(char *json_data, size_t json_len, proxy_list_t *out);
int singbox_parse_rules(char *json_data, size_t json_len, rule_list_t *out);
int singbox_parse_connections(char *json_data, size_t json_len, traffic_snapshot_t *out);
int singbox_parse_version(char *json_data, size_t json_len, char *version_buf, size_t buf_size);

void proxy_list_free(proxy_list_t *list);
void rule_list_free(rule_list_t *list);

#endif
