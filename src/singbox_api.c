#include "singbox_api.h"
#include "logger.h"
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>

static int extract_delay(yyjson_val *history) {
    if (!yyjson_is_arr(history)) return -1;
    size_t len = yyjson_arr_size(history);
    if (len == 0) return -1;
    yyjson_val *last = yyjson_arr_get(history, len - 1);
    yyjson_val *delay_val = yyjson_obj_get(last, "delay");
    return delay_val ? yyjson_get_int(delay_val) : -1;
}

int singbox_parse_proxies(char *json_data, size_t json_len, proxy_list_t *out) {
    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *proxies = yyjson_obj_get(root, "proxies");
    if (!proxies || !yyjson_is_obj(proxies)) {
        yyjson_doc_free(doc);
        return -1;
    }

    memset(out, 0, sizeof(proxy_list_t));
    out->proxies = calloc(MAX_PROXY_ITEMS, sizeof(proxy_info_t));
    if (!out->proxies) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *key, *val;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(proxies, &iter);

    while ((key = yyjson_obj_iter_next(&iter)) && out->count < MAX_PROXY_ITEMS) {
        val = yyjson_obj_iter_get_val(key);
        proxy_info_t *info = &out->proxies[out->count];

        info->name = strdup(yyjson_get_str(yyjson_obj_get(val, "name")));
        info->type = strdup(yyjson_get_str(yyjson_obj_get(val, "type")));
        info->delay = extract_delay(yyjson_obj_get(val, "history"));
        info->udp = yyjson_get_bool(yyjson_obj_get(val, "udp"));
        info->updated_at = time(NULL);

        yyjson_val *now = yyjson_obj_get(val, "now");
        if (now && yyjson_is_str(now)) {
            info->now = strdup(yyjson_get_str(now));
        }

        yyjson_val *all = yyjson_obj_get(val, "all");
        if (all && yyjson_is_arr(all)) {
            size_t all_len = yyjson_arr_size(all);
            info->all = calloc(all_len + 1, sizeof(char *));
            for (size_t i = 0; i < all_len && i < MAX_ALL_ITEMS; i++) {
                yyjson_val *item = yyjson_arr_get(all, i);
                if (yyjson_is_str(item)) {
                    info->all[info->all_count++] = strdup(yyjson_get_str(item));
                }
            }
        }

        out->count++;
    }

    yyjson_doc_free(doc);
    return out->count;
}

int singbox_parse_rules(char *json_data, size_t json_len, rule_list_t *out) {
    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *rules = yyjson_obj_get(root, "rules");
    if (!rules || !yyjson_is_arr(rules)) {
        yyjson_doc_free(doc);
        return -1;
    }

    memset(out, 0, sizeof(rule_list_t));
    out->rules = calloc(MAX_RULE_ITEMS, sizeof(rule_info_t));
    if (!out->rules) {
        yyjson_doc_free(doc);
        return -1;
    }

    size_t idx, max;
    yyjson_val *val;
    yyjson_arr_foreach(rules, idx, max, val) {
        if (out->count >= MAX_RULE_ITEMS) break;
        rule_info_t *info = &out->rules[out->count];

        yyjson_val *type = yyjson_obj_get(val, "type");
        yyjson_val *payload = yyjson_obj_get(val, "payload");
        yyjson_val *proxy = yyjson_obj_get(val, "proxy");
        yyjson_val *uuid = yyjson_obj_get(val, "uuid");

        if (type) info->type = strdup(yyjson_get_str(type));
        if (payload) info->payload = strdup(yyjson_get_str(payload));
        if (proxy) info->proxy = strdup(yyjson_get_str(proxy));
        if (uuid) info->uuid = strdup(yyjson_get_str(uuid));

        out->count++;
    }

    yyjson_doc_free(doc);
    return out->count;
}

int singbox_parse_connections(char *json_data, size_t json_len, traffic_snapshot_t *out) {
    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);

    out->upload_total = yyjson_get_uint(yyjson_obj_get(root, "uploadTotal"));
    out->download_total = yyjson_get_uint(yyjson_obj_get(root, "downloadTotal"));

    yyjson_val *connections = yyjson_obj_get(root, "connections");
    out->connection_count = yyjson_is_arr(connections) ? yyjson_arr_size(connections) : 0;
    out->timestamp = time(NULL);

    yyjson_doc_free(doc);
    return 0;
}

int singbox_parse_version(char *json_data, size_t json_len, char *version_buf, size_t buf_size) {
    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *version = yyjson_get_str(yyjson_obj_get(root, "version"));

    if (version && version_buf && buf_size > 0) {
        strncpy(version_buf, version, buf_size - 1);
        version_buf[buf_size - 1] = '\0';
    }

    yyjson_doc_free(doc);
    return version ? 0 : -1;
}

void proxy_list_free(proxy_list_t *list) {
    if (!list || !list->proxies) return;
    for (int i = 0; i < list->count; i++) {
        free(list->proxies[i].name);
        free(list->proxies[i].type);
        free(list->proxies[i].now);
        for (int j = 0; j < list->proxies[i].all_count; j++) {
            free(list->proxies[i].all[j]);
        }
        free(list->proxies[i].all);
    }
    free(list->proxies);
    list->proxies = NULL;
    list->count = 0;
}

void rule_list_free(rule_list_t *list) {
    if (!list || !list->rules) return;
    for (int i = 0; i < list->count; i++) {
        free(list->rules[i].type);
        free(list->rules[i].payload);
        free(list->rules[i].proxy);
        free(list->rules[i].uuid);
    }
    free(list->rules);
    list->rules = NULL;
    list->count = 0;
}
