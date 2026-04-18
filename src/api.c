#include "api.h"
#include "logger.h"
#include "utils.h"
#include <curl/curl.h>
#include <time.h>
#include <cjson/cJSON.h>

struct api_response {
    char *data;
    size_t size;
};

static size_t api_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct api_response *resp = (struct api_response*)userp;
    
    char *ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) return 0;
    
    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = 0;
    
    return realsize;
}

int api_init(api_ctx_t *ctx, atp_config_t *cfg) {
    memset(ctx, 0, sizeof(api_ctx_t));
    
    /* Use configured host and port */
    snprintf(ctx->base_url, sizeof(ctx->base_url), "http://%s:%d", 
             cfg->api_host, cfg->api_port);
    
    if (cfg->clash_secret[0] != '\0') {
        strncpy(ctx->secret, cfg->clash_secret, sizeof(ctx->secret) - 1);
        ctx->secret[sizeof(ctx->secret) - 1] = '\0';
    }
    
    ctx->retry_count = API_RETRY_COUNT;
    ctx->retry_delay_ms = API_RETRY_DELAY_MS;
    ctx->min_interval_ms = API_MIN_INTERVAL_MS;
    ctx->last_call_time = 0;
    ctx->last_http_code = 0;
    ctx->last_error[0] = '\0';
    ctx->timeout_sec = 2;  /* 默认 2 秒超时 */
    
    LOG_INFO("API initialized: %s (secret=%s, timeout=%ds)", 
             ctx->base_url, ctx->secret[0] ? "configured" : "not set", ctx->timeout_sec);
    return 0;
}

int api_check_rate_limit(api_ctx_t *ctx) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    
    long long elapsed_ms = now_ms - ctx->last_call_time;
    
    if (elapsed_ms < ctx->min_interval_ms && ctx->last_call_time > 0) {
        LOG_DEBUG("API rate limit: %lld ms elapsed, need %d ms", 
                  elapsed_ms, ctx->min_interval_ms);
        return -1;
    }
    
    ctx->last_call_time = now_ms;
    return 0;
}

void api_reset_rate_limit(api_ctx_t *ctx) {
    ctx->last_call_time = 0;
    LOG_DEBUG("API rate limit reset");
}

/* 带超时的 API 请求 */
static int api_do_request_with_timeout(api_ctx_t *ctx, const char *method, 
                                        const char *path, const char *body,
                                        struct api_response *response, int timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(ctx->last_error, sizeof(ctx->last_error), "Failed to init curl");
        return -1;
    }
    
    char url[512];
    snprintf(url, sizeof(url), "%s%s", ctx->base_url, path);
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    if (ctx->secret[0] != '\0') {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", ctx->secret);
        headers = curl_slist_append(headers, auth_header);
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ATPd/" ATP_VERSION);
    
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    
    if (response) {
        response->data = NULL;
        response->size = 0;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, api_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)response);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res == CURLE_OK) {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        ctx->last_http_code = (int)http_code;
    } else {
        snprintf(ctx->last_error, sizeof(ctx->last_error), "curl error: %s", 
                 curl_easy_strerror(res));
        ctx->last_http_code = 0;
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return (res == CURLE_OK) ? 0 : -1;
}

static int api_do_request(api_ctx_t *ctx, const char *method, const char *path, 
                          const char *body, struct api_response *response) {
    return api_do_request_with_timeout(ctx, method, path, body, response, ctx->timeout_sec);
}

int api_patch_json(api_ctx_t *ctx, const char *path, const char *json_body) {
    struct api_response resp;
    int ret = api_do_request(ctx, "PATCH", path, json_body, &resp);
    
    if (ret == 0 && (ctx->last_http_code == 200 || ctx->last_http_code == 204)) {
        LOG_DEBUG("API PATCH %s succeeded (HTTP %d)", path, ctx->last_http_code);
        return 0;
    }
    
    LOG_WARN("API PATCH %s failed: HTTP %d, %s", 
             path, ctx->last_http_code, ctx->last_error);
    return -1;
}

int api_get_json(api_ctx_t *ctx, const char *path, char *output, size_t size) {
    struct api_response resp;
    int ret = api_do_request(ctx, "GET", path, NULL, &resp);
    
    if (ret == 0 && ctx->last_http_code == 200 && resp.data) {
        strncpy(output, resp.data, size - 1);
        output[size - 1] = '\0';
        free(resp.data);
        return 0;
    }
    
    if (resp.data) free(resp.data);
    return -1;
}

int api_set_mode(api_ctx_t *ctx, const char *mode) {
    if (api_check_rate_limit(ctx) != 0) {
        LOG_DEBUG("API rate limited, skipping mode sync");
        return 0;
    }
    
    char json_body[256];
    snprintf(json_body, sizeof(json_body), "{\"mode\":\"%s\"}", mode);
    
    for (int i = 0; i < ctx->retry_count; i++) {
        if (i > 0) {
            LOG_DEBUG("API retry %d/%d after %d ms", i + 1, ctx->retry_count, ctx->retry_delay_ms);
            usleep(ctx->retry_delay_ms * 1000);
        }
        
        if (api_patch_json(ctx, "/configs", json_body) == 0) {
            LOG_INFO("API sync: mode set to [%s]", mode);
            
            pthread_mutex_lock(&g_config.config_mutex);
            strncpy(g_config.user_clash_mode, mode, sizeof(g_config.user_clash_mode) - 1);
            g_config.user_clash_mode[sizeof(g_config.user_clash_mode) - 1] = '\0';
            pthread_mutex_unlock(&g_config.config_mutex);
            
            return 0;
        }
    }
    
    LOG_WARN("API sync failed after %d retries", ctx->retry_count);
    return -1;
}

int api_set_mode_by_enum(api_ctx_t *ctx, api_mode_t mode) {
    const char *mode_str = api_mode_to_string(mode);
    return api_set_mode(ctx, mode_str);
}

int api_get_config(api_ctx_t *ctx, char *output, size_t size) {
    return api_get_json(ctx, "/configs", output, size);
}

int api_get_rules(api_ctx_t *ctx, char *output, size_t size) {
    return api_get_json(ctx, "/rules", output, size);
}

int api_get_proxies(api_ctx_t *ctx, char *output, size_t size) {
    return api_get_json(ctx, "/proxies", output, size);
}

int api_get_health(api_ctx_t *ctx, char *output, size_t size) {
    return api_get_json(ctx, "/health", output, size);
}

int api_get_mode(api_ctx_t *ctx, char *mode, size_t size) {
    if (!ctx || !mode || size == 0) {
        return -1;
    }
    
    struct api_response resp;
    int ret = api_do_request(ctx, "GET", "/configs", NULL, &resp);
    
    if (ret != 0 || ctx->last_http_code != 200 || !resp.data) {
        LOG_WARN("Failed to get mode from API: HTTP %d", ctx->last_http_code);
        if (resp.data) free(resp.data);
        return -1;
    }
    
    cJSON *json = cJSON_Parse(resp.data);
    free(resp.data);
    
    if (!json) {
        LOG_WARN("Failed to parse JSON response");
        return -1;
    }
    
    cJSON *mode_item = cJSON_GetObjectItem(json, "mode");
    if (!mode_item || !cJSON_IsString(mode_item)) {
        LOG_WARN("No 'mode' field in JSON response");
        cJSON_Delete(json);
        return -1;
    }
    
    strncpy(mode, mode_item->valuestring, size - 1);
    mode[size - 1] = '\0';
    
    cJSON_Delete(json);
    return 0;
}

const char *api_mode_to_string(api_mode_t mode) {
    switch (mode) {
        case API_MODE_RULE:      return "Rule";
        case API_MODE_GLOBAL:    return "Global";
        case API_MODE_DIRECT:    return "Direct";
        case API_MODE_GOOGLE_VPN: return "Google VPN";
        default:                 return "Rule";
    }
}

api_mode_t api_string_to_mode(const char *str) {
    if (strcmp(str, "Global") == 0) return API_MODE_GLOBAL;
    if (strcmp(str, "Direct") == 0) return API_MODE_DIRECT;
    if (strcmp(str, "Google VPN") == 0) return API_MODE_GOOGLE_VPN;
    return API_MODE_RULE;
}

/* 新增：确认配置已加载 */
int api_wait_for_config_loaded(api_ctx_t *ctx, const char *expected_mode, int timeout_sec) {
    char current_mode[64];
    int waited_ms = 0;
    
    LOG_DEBUG("Waiting for config confirmation (mode=%s, timeout=%ds)", 
              expected_mode, timeout_sec);
    
    while (waited_ms < timeout_sec * 1000) {
        if (api_get_mode(ctx, current_mode, sizeof(current_mode)) == 0) {
            if (strcmp(current_mode, expected_mode) == 0) {
                LOG_INFO("API: Configuration confirmed (mode=%s)", current_mode);
                return 0;
            }
        }
        usleep(200000);  /* 200ms */
        waited_ms += 200;
    }
    
    LOG_WARN("API: Config confirmation timeout after %d seconds", timeout_sec);
    return -1;
}

int api_check_health(api_ctx_t *ctx) {
    struct api_response resp;
    int ret = api_do_request_with_timeout(ctx, "GET", "/health", NULL, &resp, 2);
    
    if (ret == 0 && ctx->last_http_code == 200) {
        return 1;
    }
    return 0;
}
