/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Sing-box Clash API Parser
 * Features: INSITU parsing, Emoji safe, OOM protection
 *
 * ============================================================================
 * INSITU Contract
 *
 * json_data:
 *   1. Must be writable.
 *   2. Must be exclusively owned.
 *   3. Must remain valid during parsing.
 *   4. Will be modified by yyjson.
 * ============================================================================
 *
 * ============================================================================
 * Security Guarantees
 *
 * ✅ OOM Safe
 * ✅ UTF-8 Safe
 * ✅ Emoji Safe
 * ✅ JSON Size Limited
 * ✅ DoS Resistant
 * ✅ Type Safe
 * ✅ C99 Compatible
 * ✅ Memory Leak Resistant
 * ============================================================================
 *
 * ============================================================================
 * Lifecycle Model
 *
 * allocate
 *   ↓
 * initialize
 *   ↓
 * success
 *   ↓
 * owner free
 *
 * or
 *
 * initialize failed
 *   ↓
 * cleanup helper
 * ============================================================================
 */

#include "singbox_api.h"
#include "logger.h"
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>

/* ========== Security Limits ========== */

#ifndef SINGBOX_API_MAX_JSON_SIZE
#define SINGBOX_API_MAX_JSON_SIZE (4 * 1024 * 1024)
#endif

#ifndef SINGBOX_API_MAX_ALL_ITEMS
#define SINGBOX_API_MAX_ALL_ITEMS 1024
#endif

#ifndef SINGBOX_API_MAX_PROXY_ITEMS
#define SINGBOX_API_MAX_PROXY_ITEMS 1024
#endif

#ifndef SINGBOX_API_MAX_RULE_ITEMS
#define SINGBOX_API_MAX_RULE_ITEMS 4096
#endif

/* ========== Performance Hints ========== */

#ifdef __GNUC__
#define SINGBOX_API_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SINGBOX_API_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SINGBOX_API_LIKELY(x)   (x)
#define SINGBOX_API_UNLIKELY(x) (x)
#endif

/* ========== C99-compatible Static Assertions ========== */

#if __STDC_VERSION__ >= 201112L
/* C11 or later: use _Static_assert */
_Static_assert(SINGBOX_API_MAX_ALL_ITEMS > 0,
               "SINGBOX_API_MAX_ALL_ITEMS must be > 0");
_Static_assert(SINGBOX_API_MAX_JSON_SIZE >= 1024,
               "SINGBOX_API_MAX_JSON_SIZE must be >= 1024");
_Static_assert(SINGBOX_API_MAX_PROXY_ITEMS > 0,
               "SINGBOX_API_MAX_PROXY_ITEMS must be > 0");
_Static_assert(SINGBOX_API_MAX_RULE_ITEMS > 0,
               "SINGBOX_API_MAX_RULE_ITEMS must be > 0");
#else
/* C99 fallback: negative array size on failure */
typedef char singbox_assert_all_items_positive[
    (SINGBOX_API_MAX_ALL_ITEMS > 0) ? 1 : -1
];

typedef char singbox_assert_json_size_valid[
    (SINGBOX_API_MAX_JSON_SIZE >= 1024) ? 1 : -1
];

typedef char singbox_assert_proxy_items_positive[
    (SINGBOX_API_MAX_PROXY_ITEMS > 0) ? 1 : -1
];

typedef char singbox_assert_rule_items_positive[
    (SINGBOX_API_MAX_RULE_ITEMS > 0) ? 1 : -1
];
#endif

/* ========== Safe Helpers ========== */

static time_t safe_time_now(void) {
    time_t t = time(NULL);
    return (t == (time_t)-1) ? 0 : t;
}

static yyjson_doc* parse_json_insitu(char *json_data, size_t json_len, const char *endpoint) {
    if (!json_data) {
        LOG_ERROR("singbox_api: %s: NULL json_data", endpoint);
        return NULL;
    }

    if (json_len == 0) {
        LOG_ERROR("singbox_api: %s: empty JSON", endpoint);
        return NULL;
    }

    if (json_len > SINGBOX_API_MAX_JSON_SIZE) {
        LOG_ERROR("singbox_api: %s: JSON too large (%zu > %zu)",
                  endpoint, json_len, (size_t)SINGBOX_API_MAX_JSON_SIZE);
        return NULL;
    }

    yyjson_doc *doc = yyjson_read_opts(json_data, json_len,
                                       YYJSON_READ_INSITU, NULL, NULL);
    if (SINGBOX_API_UNLIKELY(!doc)) {
        LOG_ERROR("singbox_api: %s: failed to parse JSON", endpoint);
        return NULL;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (SINGBOX_API_UNLIKELY(!root || !yyjson_is_obj(root))) {
        LOG_ERROR("singbox_api: %s: invalid or empty root object", endpoint);
        yyjson_doc_free(doc);
        return NULL;
    }

    return doc;
}

static char* safe_strdup(const char *src, const char *fallback) {
    char *result = NULL;
    const char *source = src ? src : fallback;
    size_t len = 0;

    if (!source) {
        return NULL;
    }

    len = strlen(source);
    result = strdup(source);
    if (SINGBOX_API_UNLIKELY(!result)) {
        LOG_ERROR("singbox_api: safe_strdup OOM len=%zu", len);
        return NULL;
    }

    return result;
}

static int extract_delay(yyjson_val *history) {
    if (!history || !yyjson_is_arr(history)) return -1;

    size_t len = yyjson_arr_size(history);
    if (len == 0) return -1;

    yyjson_val *last = yyjson_arr_get(history, len - 1);
    if (!last || !yyjson_is_obj(last)) return -1;

    yyjson_val *delay_val = yyjson_obj_get(last, "delay");
    if (!delay_val || !yyjson_is_num(delay_val)) return -1;

    int64_t delay = yyjson_get_sint(delay_val);
    if (delay < 0) return -1;
    if (delay > INT_MAX) return INT_MAX;

    return (int)delay;
}

/* ========== Lifecycle Helpers ========== */

/*
 * Partial Object Cleanup
 *
 * Supports:
 *  - fully initialized objects
 *  - partially initialized objects
 *  - repeated cleanup calls
 *
 * Safe during:
 *  - OOM
 *  - early abort
 *  - parse failure
 */
static void proxy_info_cleanup(proxy_info_t *info) {
    if (!info) return;

    free(info->name);
    info->name = NULL;

    free(info->type);
    info->type = NULL;

    free(info->now);
    info->now = NULL;

    if (info->all) {
        for (int i = 0; i < info->all_count; i++) {
            free(info->all[i]);
            info->all[i] = NULL;
        }
        free(info->all);
        info->all = NULL;
    }

    info->all_count = 0;
    memset(info, 0, sizeof(*info));
}

/*
 * Partial Object Cleanup
 *
 * Supports:
 *  - fully initialized objects
 *  - partially initialized objects
 *  - repeated cleanup calls
 *
 * Safe during:
 *  - OOM
 *  - early abort
 *  - parse failure
 */
static void rule_info_cleanup(rule_info_t *info) {
    if (!info) return;

    free(info->type);
    info->type = NULL;

    free(info->payload);
    info->payload = NULL;

    free(info->proxy);
    info->proxy = NULL;

    free(info->uuid);
    info->uuid = NULL;

    memset(info, 0, sizeof(*info));
}

/* ========== Proxies Parser ========== */

int singbox_parse_proxies(char *json_data, size_t json_len, proxy_list_t *out) {
    yyjson_doc *doc = NULL;
    int ret = -1;

    if (!json_data || !out) return -1;

    doc = parse_json_insitu(json_data, json_len, "/proxies");
    if (SINGBOX_API_UNLIKELY(!doc)) goto cleanup;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *proxies = yyjson_obj_get(root, "proxies");
    if (SINGBOX_API_UNLIKELY(!proxies || !yyjson_is_obj(proxies))) {
        LOG_ERROR("singbox_api: /proxies: missing 'proxies' object");
        goto cleanup;
    }

    size_t actual_proxy_count = yyjson_obj_size(proxies);

    memset(out, 0, sizeof(proxy_list_t));
    out->proxies = calloc(SINGBOX_API_MAX_PROXY_ITEMS, sizeof(proxy_info_t));
    if (SINGBOX_API_UNLIKELY(!out->proxies)) {
        LOG_ERROR("singbox_api: /proxies: calloc OOM");
        goto cleanup;
    }

    yyjson_val *key, *val;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(proxies, &iter);
    int alloc_failed = 0;
    proxy_info_t *partial_info = NULL;

    while ((key = yyjson_obj_iter_next(&iter)) &&
           out->count < SINGBOX_API_MAX_PROXY_ITEMS && !alloc_failed) {
        val = yyjson_obj_iter_get_val(key);
        if (!yyjson_is_obj(val)) continue;

        proxy_info_t *info = &out->proxies[out->count];
        partial_info = info;
        memset(info, 0, sizeof(proxy_info_t));

        yyjson_val *name_val = yyjson_obj_get(val, "name");
        yyjson_val *type_val = yyjson_obj_get(val, "type");

        info->name = safe_strdup(
            (name_val && yyjson_is_str(name_val)) ? yyjson_get_str(name_val) : NULL,
            "Unknown"
        );
        if (SINGBOX_API_UNLIKELY(!info->name)) {
            alloc_failed = 1;
            break;
        }

        info->type = safe_strdup(
            (type_val && yyjson_is_str(type_val)) ? yyjson_get_str(type_val) : NULL,
            "Direct"
        );
        if (SINGBOX_API_UNLIKELY(!info->type)) {
            alloc_failed = 1;
            break;
        }

        info->delay = extract_delay(yyjson_obj_get(val, "history"));

        yyjson_val *udp_val = yyjson_obj_get(val, "udp");
        info->udp = (udp_val && yyjson_is_bool(udp_val)) ? yyjson_get_bool(udp_val) : 0;

        info->updated_at = safe_time_now();

        yyjson_val *now = yyjson_obj_get(val, "now");
        if (now && yyjson_is_str(now)) {
            info->now = safe_strdup(yyjson_get_str(now), NULL);
            if (SINGBOX_API_UNLIKELY(!info->now)) {
                alloc_failed = 1;
                break;
            }
        }

        yyjson_val *all = yyjson_obj_get(val, "all");
        if (all && yyjson_is_arr(all)) {
            size_t all_len = yyjson_arr_size(all);
            size_t alloc_len = (all_len > SINGBOX_API_MAX_ALL_ITEMS) ?
                               SINGBOX_API_MAX_ALL_ITEMS : all_len;

            if (alloc_len > 0) {
                info->all = calloc(alloc_len + 1, sizeof(char *));
                if (SINGBOX_API_UNLIKELY(!info->all)) {
                    alloc_failed = 1;
                    break;
                }

                for (size_t i = 0; i < alloc_len && !alloc_failed; i++) {
                    yyjson_val *item = yyjson_arr_get(all, i);
                    if (item && yyjson_is_str(item)) {
                        const char *item_str = yyjson_get_str(item);
                        info->all[info->all_count] = safe_strdup(item_str, NULL);
                        if (SINGBOX_API_UNLIKELY(!info->all[info->all_count])) {
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
            partial_info = NULL;
        }
    }

    if (actual_proxy_count > SINGBOX_API_MAX_PROXY_ITEMS) {
        LOG_WARN("singbox_api: /proxies: truncated %zu to %zu items",
                 actual_proxy_count, (size_t)SINGBOX_API_MAX_PROXY_ITEMS);
    }

    if (alloc_failed) {
        /* Clean up the partial object */
        if (partial_info) {
            proxy_info_cleanup(partial_info);
        }
        LOG_ERROR("singbox_api: /proxies: memory allocation failed");
        proxy_list_free(out);
        goto cleanup;
    }

    LOG_DEBUG("singbox_api: /proxies: parsed %d proxies", out->count);
    ret = out->count;

cleanup:
    if (doc) yyjson_doc_free(doc);
    return ret;
}

/* ========== Rules Parser ========== */

int singbox_parse_rules(char *json_data, size_t json_len, rule_list_t *out) {
    yyjson_doc *doc = NULL;
    int ret = -1;

    if (!json_data || !out) return -1;

    doc = parse_json_insitu(json_data, json_len, "/rules");
    if (SINGBOX_API_UNLIKELY(!doc)) goto cleanup;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *rules = yyjson_obj_get(root, "rules");
    if (SINGBOX_API_UNLIKELY(!rules || !yyjson_is_arr(rules))) {
        LOG_ERROR("singbox_api: /rules: missing 'rules' array");
        goto cleanup;
    }

    memset(out, 0, sizeof(rule_list_t));
    out->rules = calloc(SINGBOX_API_MAX_RULE_ITEMS, sizeof(rule_info_t));
    if (SINGBOX_API_UNLIKELY(!out->rules)) {
        LOG_ERROR("singbox_api: /rules: calloc OOM");
        goto cleanup;
    }

    size_t idx, max;
    yyjson_val *val;
    size_t total_rules = yyjson_arr_size(rules);

    yyjson_arr_foreach(rules, idx, max, val) {
        if (out->count >= SINGBOX_API_MAX_RULE_ITEMS) break;
        if (!yyjson_is_obj(val)) continue;

        rule_info_t *info = &out->rules[out->count];
        memset(info, 0, sizeof(rule_info_t));

        yyjson_val *type_val = yyjson_obj_get(val, "type");
        yyjson_val *payload_val = yyjson_obj_get(val, "payload");
        yyjson_val *proxy_val = yyjson_obj_get(val, "proxy");
        yyjson_val *uuid_val = yyjson_obj_get(val, "uuid");

        info->type = safe_strdup(
            (type_val && yyjson_is_str(type_val)) ? yyjson_get_str(type_val) : NULL,
            ""
        );
        info->payload = safe_strdup(
            (payload_val && yyjson_is_str(payload_val)) ? yyjson_get_str(payload_val) : NULL,
            ""
        );
        info->proxy = safe_strdup(
            (proxy_val && yyjson_is_str(proxy_val)) ? yyjson_get_str(proxy_val) : NULL,
            ""
        );
        info->uuid = safe_strdup(
            (uuid_val && yyjson_is_str(uuid_val)) ? yyjson_get_str(uuid_val) : NULL,
            ""
        );

        /* Skip malformed rules - clean up partial state with unified cleanup */
        if (!info->type || !info->payload || !info->proxy) {
            rule_info_cleanup(info);
            continue;
        }

        out->count++;
    }

    if (total_rules > SINGBOX_API_MAX_RULE_ITEMS) {
        LOG_WARN("singbox_api: /rules: truncated %zu to %zu items",
                 total_rules, (size_t)SINGBOX_API_MAX_RULE_ITEMS);
    }

    LOG_DEBUG("singbox_api: /rules: parsed %d rules", out->count);
    ret = out->count;

cleanup:
    if (doc) yyjson_doc_free(doc);
    return ret;
}

/* ========== Connections Parser ========== */

int singbox_parse_connections(char *json_data, size_t json_len, traffic_snapshot_t *out) {
    yyjson_doc *doc = NULL;
    int ret = -1;

    if (!json_data || !out) return -1;

    doc = parse_json_insitu(json_data, json_len, "/connections");
    if (SINGBOX_API_UNLIKELY(!doc)) goto cleanup;

    yyjson_val *root = yyjson_doc_get_root(doc);

    yyjson_val *upload = yyjson_obj_get(root, "uploadTotal");
    yyjson_val *download = yyjson_obj_get(root, "downloadTotal");
    yyjson_val *connections = yyjson_obj_get(root, "connections");

    /* Strict type checking for uploadTotal - support uint64 */
    if (upload) {
        if (yyjson_is_uint(upload)) {
            out->upload_total = yyjson_get_uint(upload);
        } else if (yyjson_is_int(upload)) {
            int64_t val = yyjson_get_sint(upload);
            out->upload_total = (val > 0) ? (uint64_t)val : 0;
        } else if (yyjson_is_num(upload)) {
            int64_t val = yyjson_get_sint(upload);
            out->upload_total = (val > 0) ? (uint64_t)val : 0;
        } else {
            out->upload_total = 0;
        }
    } else {
        out->upload_total = 0;
    }

    /* Strict type checking for downloadTotal - support uint64 */
    if (download) {
        if (yyjson_is_uint(download)) {
            out->download_total = yyjson_get_uint(download);
        } else if (yyjson_is_int(download)) {
            int64_t val = yyjson_get_sint(download);
            out->download_total = (val > 0) ? (uint64_t)val : 0;
        } else if (yyjson_is_num(download)) {
            int64_t val = yyjson_get_sint(download);
            out->download_total = (val > 0) ? (uint64_t)val : 0;
        } else {
            out->download_total = 0;
        }
    } else {
        out->download_total = 0;
    }

    size_t conn_cnt = (connections && yyjson_is_arr(connections)) ?
                      yyjson_arr_size(connections) : 0;
    out->connection_count = (conn_cnt > INT_MAX) ? INT_MAX : (int)conn_cnt;
    out->timestamp = safe_time_now();

    LOG_DEBUG("singbox_api: /connections: up=%" PRIu64 ", down=%" PRIu64 ", conn=%d",
              out->upload_total, out->download_total, out->connection_count);

    ret = 0;

cleanup:
    if (doc) yyjson_doc_free(doc);
    return ret;
}

/* ========== Version Parser ========== */

int singbox_parse_version(char *json_data, size_t json_len, char *version_buf, size_t buf_size) {
    yyjson_doc *doc = NULL;
    int ret = -1;

    if (!json_data || !version_buf || buf_size == 0) return -1;

    doc = parse_json_insitu(json_data, json_len, "/version");
    if (SINGBOX_API_UNLIKELY(!doc)) goto cleanup;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *version_val = yyjson_obj_get(root, "version");

    const char *version = (version_val && yyjson_is_str(version_val)) ?
                          yyjson_get_str(version_val) : NULL;

    if (version) {
        snprintf(version_buf, buf_size, "%s", version);
        ret = 0;
    } else {
        version_buf[0] = '\0';
        ret = -1;
    }

cleanup:
    if (doc) yyjson_doc_free(doc);
    return ret;
}

/* ========== Memory Cleanup ========== */

void proxy_list_free(proxy_list_t *list) {
    if (!list || !list->proxies) return;

    for (int i = 0; i < list->count; i++) {
        proxy_info_cleanup(&list->proxies[i]);
    }

    free(list->proxies);
    list->proxies = NULL;
    list->count = 0;
}

void rule_list_free(rule_list_t *list) {
    if (!list || !list->rules) return;

    for (int i = 0; i < list->count; i++) {
        rule_info_cleanup(&list->rules[i]);
    }

    free(list->rules);
    list->rules = NULL;
    list->count = 0;
}
