/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * GeoIP download - Native socket implementation
 */

#include "geoip.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include "ipset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <cjson/cJSON.h>

#define GEOIP_TIMEOUT_SEC 30
#define GEOIP_BUFFER_SIZE 4096

static pthread_t geoip_thread;
static int geoip_async_running = 0;
static int geoip_async_complete = 0;

static const char *default_cn_cidrs[] = {
    "1.0.0.0/8", "14.0.0.0/8", "27.0.0.0/8", "36.0.0.0/8",
    "39.0.0.0/8", "42.0.0.0/8", "49.0.0.0/8", "58.0.0.0/8",
    "59.0.0.0/8", "60.0.0.0/8", "61.0.0.0/8", "101.0.0.0/8",
    "106.0.0.0/8", "110.0.0.0/8", "111.0.0.0/8", "112.0.0.0/8",
    "113.0.0.0/8", "114.0.0.0/8", "115.0.0.0/8", "116.0.0.0/8",
    "117.0.0.0/8", "118.0.0.0/8", "119.0.0.0/8", "120.0.0.0/8",
    "121.0.0.0/8", "122.0.0.0/8", "123.0.0.0/8", "124.0.0.0/8",
    "125.0.0.0/8", "126.0.0.0/8", "169.254.0.0/16", "172.16.0.0/12",
    "192.168.0.0/16", "223.0.0.0/8", NULL
};

struct geoip_response {
    char *data;
    size_t size;
};

static size_t geoip_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct geoip_response *resp = (struct geoip_response*)userp;
    
    char *ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) return 0;
    
    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = 0;
    
    return realsize;
}

static int geoip_parse_url(const char *url, char *host, int *port, char *path, size_t path_size) {
    const char *start;
    
    if (strncmp(url, "https://", 8) == 0) {
        LOG_ERROR("HTTPS not supported for GeoIP download");
        return -1;
    } else if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        *port = 80;
    } else {
        start = url;
        *port = 80;
    }
    
    const char *slash = strchr(start, '/');
    const char *colon = strchr(start, ':');
    
    if (colon && (!slash || colon < slash)) {
        size_t len = colon - start;
        strncpy(host, start, len);
        host[len] = '\0';
        *port = atoi(colon + 1);
        if (slash) {
            strncpy(path, slash, path_size - 1);
        } else {
            strcpy(path, "/");
        }
    } else if (slash) {
        size_t len = slash - start;
        strncpy(host, start, len);
        host[len] = '\0';
        strncpy(path, slash, path_size - 1);
    } else {
        strcpy(host, start);
        strcpy(path, "/");
    }
    
    return 0;
}

static int geoip_http_download(const char *url, struct geoip_response *resp, int timeout_sec) {
    char host[256];
    char path[1024];
    int port;
    int sock_fd = -1;
    int result = -1;
    
    if (geoip_parse_url(url, host, &port, path, sizeof(path)) != 0) {
        return -1;
    }
    
    LOG_DEBUG("GeoIP: connecting to %s:%d%s", host, port, path);
    
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_ERROR("GeoIP: socket failed: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            LOG_ERROR("GeoIP: DNS failed for %s", host);
            goto cleanup;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("GeoIP: connect failed: %s", strerror(errno));
        goto cleanup;
    }
    
    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: ATPd/1.0\r\n"
             "Accept: text/plain\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);
    
    if (send(sock_fd, request, strlen(request), 0) != (ssize_t)strlen(request)) {
        LOG_ERROR("GeoIP: send failed: %s", strerror(errno));
        goto cleanup;
    }
    
    resp->data = malloc(GEOIP_BUFFER_SIZE);
    resp->size = 0;
    size_t capacity = GEOIP_BUFFER_SIZE;
    
    while (1) {
        if (resp->size >= capacity - 1) {
            capacity *= 2;
            char *new_data = realloc(resp->data, capacity);
            if (!new_data) goto cleanup;
            resp->data = new_data;
        }
        
        ssize_t recvd = recv(sock_fd, resp->data + resp->size, capacity - resp->size - 1, 0);
        if (recvd > 0) {
            resp->size += recvd;
            resp->data[resp->size] = '\0';
        } else if (recvd == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOG_ERROR("GeoIP: recv failed: %s", strerror(errno));
            goto cleanup;
        }
    }
    
    char *body = strstr(resp->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t body_len = strlen(body);
        memmove(resp->data, body, body_len + 1);
        resp->size = body_len;
    }
    
    result = 0;
    
cleanup:
    if (sock_fd >= 0) close(sock_fd);
    if (result != 0) {
        free(resp->data);
        resp->data = NULL;
        resp->size = 0;
    }
    return result;
}

int geoip_download_url(const char *url, const char *output_path, int timeout_sec) {
    struct geoip_response resp = {0};
    
    if (geoip_http_download(url, &resp, timeout_sec) != 0) {
        return -1;
    }
    
    FILE *fp = fopen(output_path, "w");
    if (!fp) {
        free(resp.data);
        return -1;
    }
    
    fwrite(resp.data, 1, resp.size, fp);
    fclose(fp);
    free(resp.data);
    
    return 0;
}

int geoip_init(atp_config_t *cfg) {
    char rules_dir[PATH_MAX];
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    mkdir_recursive(rules_dir, 0755);
    
    LOG_DEBUG("GeoIP initialized (native socket)");
    return 0;
}

int geoip_download(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) {
        LOG_DEBUG("CN IP bypass disabled, skipping download");
        return 0;
    }
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_tmp[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_tmp, sizeof(v4_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ip_file);
    
    LOG_INFO("Downloading China IPv4 list");
    if (geoip_download_url(cfg->cn_ip_url, v4_tmp, GEOIP_TIMEOUT_SEC) == 0) {
        rename(v4_tmp, v4_path);
        LOG_INFO("IPv4 list downloaded successfully");
        return 0;
    } else {
        LOG_WARN("Failed to download IPv4 list, using cached if available");
        return -1;
    }
}

static int geoip_create_default_ipset(atp_config_t *cfg) {
    LOG_INFO("Creating default ipset (fallback mode)");
    
    geoip_ipset_destroy("cnip");
    geoip_ipset_create("cnip", 4, 8192, 65536);
    
    for (int i = 0; default_cn_cidrs[i] != NULL; i++) {
        ipset_add_entry("cnip", default_cn_cidrs[i]);
    }
    
    int count = 0;
    while (default_cn_cidrs[count] != NULL) count++;
    LOG_INFO("Default ipset 'cnip' created with %d entries", count);
    return 0;
}

static void* geoip_async_update_thread(void *arg) {
    atp_config_t *cfg = (atp_config_t*)arg;
    LOG_INFO("GeoIP async update started in background");
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_parsed[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed, sizeof(v4_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ip_file);
    
    if (geoip_download(cfg) == 0 && file_exists(v4_path)) {
        geoip_parse_cidr_file(v4_path, v4_parsed, 4);
        
        geoip_ipset_create("cnip_temp", 4, 8192, 65536);
        geoip_ipset_restore_file("cnip_temp", v4_parsed);
        geoip_ipset_swap("cnip_temp", "cnip");
        geoip_ipset_destroy("cnip_temp");
        LOG_INFO("IPv4 ipset upgraded to full list");
    } else {
        LOG_WARN("Full GeoIP download failed, keeping default list");
    }
    
    geoip_async_running = 0;
    geoip_async_complete = 1;
    LOG_INFO("GeoIP async update completed");
    return NULL;
}

int geoip_setup_ipset_async(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) {
        LOG_DEBUG("CN IP bypass disabled, skipping ipset setup");
        return 0;
    }
    
    geoip_create_default_ipset(cfg);
    
    if (!geoip_async_running) {
        geoip_async_running = 1;
        geoip_async_complete = 0;
        if (pthread_create(&geoip_thread, NULL, geoip_async_update_thread, cfg) != 0) {
            LOG_WARN("Failed to create GeoIP async thread");
            geoip_async_running = 0;
            return -1;
        }
        pthread_detach(geoip_thread);
        LOG_INFO("GeoIP async update thread started");
    }
    
    return 0;
}

int geoip_async_is_complete(void) {
    return geoip_async_complete;
}

int geoip_setup_ipset(atp_config_t *cfg) {
    return geoip_setup_ipset_async(cfg);
}

int geoip_cleanup_ipset(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    
    geoip_ipset_destroy("cnip");
    geoip_ipset_destroy("cnip6");
    
    LOG_INFO("GeoIP ipsets destroyed");
    return 0;
}

int geoip_atomic_update(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    
    LOG_INFO("Performing atomic GeoIP update");
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_tmp[PATH_MAX];
    char v4_parsed[PATH_MAX];
    char v4_parsed_tmp[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_tmp, sizeof(v4_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed, sizeof(v4_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed_tmp, sizeof(v4_parsed_tmp), "%s/%s.parsed.tmp", rules_dir, cfg->cn_ip_file);
    
    if (geoip_download_url(cfg->cn_ip_url, v4_tmp, GEOIP_TIMEOUT_SEC) != 0) {
        LOG_ERROR("Failed to download fresh IPv4 list");
        return -1;
    }
    
    geoip_parse_cidr_file(v4_tmp, v4_parsed_tmp, 4);
    geoip_ipset_create("cnip_temp", 4, 8192, 65536);
    geoip_ipset_restore_file("cnip_temp", v4_parsed_tmp);
    geoip_ipset_swap("cnip_temp", "cnip");
    geoip_ipset_destroy("cnip_temp");
    rename(v4_tmp, v4_path);
    rename(v4_parsed_tmp, v4_parsed);
    
    if (cfg->proxy_ipv6) {
        char v6_path[PATH_MAX];
        char v6_tmp[PATH_MAX];
        char v6_parsed[PATH_MAX];
        char v6_parsed_tmp[PATH_MAX];
        
        snprintf(v6_path, sizeof(v6_path), "%s/%s", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_tmp, sizeof(v6_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_parsed, sizeof(v6_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_parsed_tmp, sizeof(v6_parsed_tmp), "%s/%s.parsed.tmp", rules_dir, cfg->cn_ipv6_file);
        
        if (geoip_download_url(cfg->cn_ipv6_url, v6_tmp, GEOIP_TIMEOUT_SEC) == 0) {
            geoip_parse_cidr_file(v6_tmp, v6_parsed_tmp, 6);
            geoip_ipset_create("cnip6_temp", 6, 8192, 65536);
            geoip_ipset_restore_file("cnip6_temp", v6_parsed_tmp);
            geoip_ipset_swap("cnip6_temp", "cnip6");
            geoip_ipset_destroy("cnip6_temp");
            rename(v6_tmp, v6_path);
            rename(v6_parsed_tmp, v6_parsed);
        }
    }
    
    LOG_INFO("Atomic GeoIP update completed");
    return 0;
}

int geoip_check_update_needed(atp_config_t *cfg, int max_age_days) {
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    
    if (!file_exists(v4_path)) {
        LOG_INFO("IPv4 list missing, update needed");
        return 1;
    }
    
    struct stat st;
    if (stat(v4_path, &st) != 0) return 1;
    
    time_t now = time(NULL);
    int age_days = (now - st.st_mtime) / 86400;
    
    if (age_days >= max_age_days) {
        LOG_INFO("IPv4 list is %d days old, update needed", age_days);
        return 1;
    }
    
    LOG_DEBUG("IPv4 list is %d days old, no update needed", age_days);
    return 0;
}

int geoip_force_update(atp_config_t *cfg) {
    LOG_INFO("Forcing GeoIP update");
    return geoip_atomic_update(cfg);
}

int geoip_validate_cidr(const char *cidr, int family) {
    char ip[128];
    int prefix;
    
    if (sscanf(cidr, "%127[^/]/%d", ip, &prefix) != 2) return -1;
    
    struct in_addr ipv4;
    struct in6_addr ipv6;
    
    if (family == 4) {
        if (inet_pton(AF_INET, ip, &ipv4) == 1 && prefix >= 0 && prefix <= 32) return 0;
    } else {
        if (inet_pton(AF_INET6, ip, &ipv6) == 1 && prefix >= 0 && prefix <= 128) return 0;
    }
    
    return -1;
}
