/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Sing-box Clash API Parser
 * Features: INSITU parsing, Emoji safe, OOM protection
 */

#include "singbox_api.h"
#include "logger.h"
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========== Safe String Helpers ========== */

static char* safe_strdup(const char *src, const char *fallback) {
    if (!src) {
        return fallback ? strdup(fallback) : NULL;
    }
    return strdup(src);
}

static char* extract_string(yyjson_val *obj, const char *key, const char *fallback) {
    if (!obj) return fallback ? strdup(fallback) : NULL;
    yyjson_val *val = yyjson_obj_get(obj, key);
    if (!val || !yyjson_is_str(val)) {
        return fallback ? strdup(fallback) : NULL;
    }
    const char *str = yyjson_get_str(val);
    return str ? strdup(str) : (fallback ? strdup(fallback) : NULL);
}

static int extract_delay(yyjson_val *history) {
    if (!yyjson_is_arr(history)) return -1;
    size_t len = yyjson_arr_size(history);
    if (len == 0) return -1;
    yyjson_val *last = yyjson_arr_get(history, len - 1);
    if (!yyjson_is_obj(last)) return -1;
    yyjson_val *delay_val = yyjson_obj_get(last, "delay");
    return delay_val ? yyjson_get_int(delay_val) : -1;
}

/* ========== Proxies Parser ========== */

int singbox_parse_proxies(char *json_data, size_t json_len, proxy_list_t *out) {
    if (!json_data || !out) return -1;

    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) {
        LOG_ERROR("singbox_api: failed to parse JSON for /proxies");
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *proxies = yyjson_obj_get(root, "proxies");
    if (!proxies || !yyjson_is_obj(proxies)) {
        LOG_ERROR("singbox_api: /proxies response missing 'proxies' object");
        yyjson_doc_free(doc);
        return -1;
    }

    memset(out, 0, sizeof(proxy_list_t));
    out->proxies = calloc(MAX_PROXY_ITEMS, sizeof(proxy_info_t));
    if (!out->proxies) {
        LOG_ERROR("singbox_api: failed to allocate proxy list");
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *key, *val;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(proxies, &iter);
    int alloc_failed = 0;

    while ((key = yyjson_obj_iter_next(&iter)) && out->count < MAX_PROXY_ITEMS && !alloc_failed) {
        val = yyjson_obj_iter_get_val(key);
        if (!yyjson_is_obj(val)) continue;

        proxy_info_t *info = &out->proxies[out->count];
        memset(info, 0, sizeof(proxy_info_t));

        info->name = extract_string(val, "name", "Unknown");
        info->type = extract_string(val, "type", "Direct");

        if (!info->name || !info->type) {
            alloc_failed = 1;
            break;
        }

        info->delay = extract_delay(yyjson_obj_get(val, "history"));
        info->udp = yyjson_get_bool(yyjson_obj_get(val, "udp"));
        info->updated_at = time(NULL);

        yyjson_val *now = yyjson_obj_get(val, "now");
        if (now && yyjson_is_str(now)) {
            const char *now_str = yyjson_get_str(now);
            info->now = safe_strdup(now_str, NULL);
            if (!info->now) {
                alloc_failed = 1;
                break;
            }
        }

        yyjson_val *all = yyjson_obj_get(val, "all");
        if (all && yyjson_is_arr(all)) {
            size_t all_len = yyjson_arr_size(all);
            if (all_len > 0) {
                info->all = calloc(all_len + 1, sizeof(char *));
                if (!info->all) {
                    alloc_failed = 1;
                    break;
                }

                for (size_t i = 0; i < all_len && i < MAX_ALL_ITEMS && !alloc_failed; i++) {
                    yyjson_val *item = yyjson_arr_get(all, i);
                    if (yyjson_is_str(item)) {
                        const char *item_str = yyjson_get_str(item);
                        info->all[info->all_count] = safe_strdup(item_str, NULL);
                        if (!info->all[info->all_count]) {
                            alloc_failed = 1;
                            break;
                        }
                        info->all_count++;
                    }
                }
            }
        }

        if (!alloc_failed) {
            out->count++;
        }
    }

    yyjson_doc_free(doc);

    if (alloc_failed) {
        LOG_ERROR("singbox_api: memory allocation failed during /proxies parsing");
        proxy_list_free(out);
        return -1;
    }

    LOG_DEBUG("singbox_api: parsed %d proxies", out->count);
    return out->count;
}

/* ========== Rules Parser ========== */

int singbox_parse_rules(char *json_data, size_t json_len, rule_list_t *out) {
    if (!json_data || !out) return -1;

    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) {
        LOG_ERROR("singbox_api: failed to parse JSON for /rules");
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *rules = yyjson_obj_get(root, "rules");
    if (!rules || !yyjson_is_arr(rules)) {
        LOG_ERROR("singbox_api: /rules response missing 'rules' array");
        yyjson_doc_free(doc);
        return -1;
    }

    memset(out, 0, sizeof(rule_list_t));
    out->rules = calloc(MAX_RULE_ITEMS, sizeof(rule_info_t));
    if (!out->rules) {
        LOG_ERROR("singbox_api: failed to allocate rule list");
        yyjson_doc_free(doc);
        return -1;
    }

    size_t idx, max;
    yyjson_val *val;
    yyjson_arr_foreach(rules, idx, max, val) {
        if (out->count >= MAX_RULE_ITEMS) break;
        if (!yyjson_is_obj(val)) continue;

        rule_info_t *info = &out->rules[out->count];
        memset(info, 0, sizeof(rule_info_t));

        info->type = extract_string(val, "type", "");
        info->payload = extract_string(val, "payload", "");
        info->proxy = extract_string(val, "proxy", "");
        info->uuid = extract_string(val, "uuid", "");

        if (!info->type || !info->payload || !info->proxy) {
            free(info->type);
            free(info->payload);
            free(info->proxy);
            free(info->uuid);
            continue;
        }

        out->count++;
    }

    yyjson_doc_free(doc);
    LOG_DEBUG("singbox_api: parsed %d rules", out->count);
    return out->count;
}

/* ========== Connections Parser ========== */

int singbox_parse_connections(char *json_data, size_t json_len, traffic_snapshot_t *out) {
    if (!json_data || !out) return -1;

    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) {
        LOG_ERROR("singbox_api: failed to parse JSON for /connections");
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *upload = yyjson_obj_get(root, "uploadTotal");
    yyjson_val *download = yyjson_obj_get(root, "downloadTotal");
    yyjson_val *connections = yyjson_obj_get(root, "connections");

    out->upload_total = upload ? yyjson_get_uint(upload) : 0;
    out->download_total = download ? yyjson_get_uint(download) : 0;
    out->connection_count = yyjson_is_arr(connections) ? yyjson_arr_size(connections) : 0;
    out->timestamp = time(NULL);

    yyjson_doc_free(doc);
    LOG_DEBUG("singbox_api: traffic snapshot - up=%lu, down=%lu, conn=%d",
              out->upload_total, out->download_total, out->connection_count);
    return 0;
}

/* ========== Version Parser ========== */

int singbox_parse_version(char *json_data, size_t json_len, char *version_buf, size_t buf_size) {
    if (!json_data || !version_buf || buf_size == 0) return -1;

    yyjson_read_flag flg = YYJSON_READ_INSITU;
    yyjson_doc *doc = yyjson_read_opts(json_data, json_len, flg, NULL, NULL);
    if (!doc) {
        LOG_ERROR("singbox_api: failed to parse JSON for /version");
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *version = yyjson_get_str(yyjson_obj_get(root, "version"));

    if (version) {
        strncpy(version_buf, version, buf_size - 1);
        version_buf[buf_size - 1] = '\0';
    } else {
        version_buf[0] = '\0';
    }

    yyjson_doc_free(doc);
    return version ? 0 : -1;
}

/* ========== Memory Cleanup ========== */

void proxy_list_free(proxy_list_t *list) {
    if (!list || !list->proxies) return;

    for (int i = 0; i < list->count; i++) {
        proxy_info_t *info = &list->proxies[i];
        free(info->name);
        free(info->type);
        free(info->now);
        for (int j = 0; j < info->all_count; j++) {
            free(info->all[j]);
        }
        free(info->all);
    }

    free(list->proxies);
    list->proxies = NULL;
    list->count = 0;
}

void rule_list_free(rule_list_t *list) {
    if (!list || !list->rules) return;

    for (int i = 0; i < list->count; i++) {
        rule_info_t *info = &list->rules[i];
        free(info->type);
        free(info->payload);
        free(info->proxy);
        free(info->uuid);
    }

    free(list->rules);
    list->rules = NULL;
    list->count = 0;
}
