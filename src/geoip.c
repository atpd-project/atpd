#include "geoip.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include "ipset.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

struct curl_memory {
    char *data;
    size_t size;
};

/* Built-in default China IPv4 CIDRs (fallback when download fails) */
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

static pthread_t geoip_thread;
static int geoip_async_running = 0;
static int geoip_async_complete = 0;

static size_t curl_mem_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct curl_memory *mem = (struct curl_memory*)userp;
    
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;
    
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    
    return realsize;
}

int geoip_init(atp_config_t *cfg) {
    char rules_dir[PATH_MAX];
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    mkdir_recursive(rules_dir, 0755);
    
    LOG_DEBUG("GeoIP initialized");
    return 0;
}

int geoip_download_url(const char *url, const char *output_path, int timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize curl");
        return -1;
    }
    
    struct curl_memory chunk = {0};
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_mem_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ATPd/" ATP_VERSION);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res == CURLE_OK && chunk.data && chunk.size > 0) {
        FILE *fp = fopen(output_path, "w");
        if (fp) {
            fwrite(chunk.data, 1, chunk.size, fp);
            fclose(fp);
            LOG_DEBUG("Downloaded %zu bytes from %s", chunk.size, url);
            free(chunk.data);
            curl_easy_cleanup(curl);
            return 0;
        }
    }
    
    LOG_ERROR("Failed to download %s: %s", url, curl_easy_strerror(res));
    free(chunk.data);
    curl_easy_cleanup(curl);
    return -1;
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
    if (geoip_download_url(cfg->cn_ip_url, v4_tmp, 30) == 0) {
        rename(v4_tmp, v4_path);
        LOG_INFO("IPv4 list downloaded successfully");
        return 0;
    } else {
        LOG_WARN("Failed to download IPv4 list, using cached if available");
        return -1;
    }
}

/* Wrapper functions that call ipset.c implementation */
int geoip_ipset_create(const char *name, int family, int hashsize, int maxelem) {
    return ipset_create(name, family, hashsize, maxelem);
}

int geoip_ipset_destroy(const char *name) {
    return ipset_destroy(name);
}

int geoip_ipset_swap(const char *from, const char *to) {
    return ipset_swap(from, to);
}

int geoip_ipset_exists(const char *name) {
    return ipset_exists(name);
}

int geoip_ipset_flush(const char *name) {
    return ipset_flush(name);
}

int geoip_ipset_restore_file(const char *name, const char *filename) {
    return ipset_restore_file(name, filename);
}

int geoip_parse_cidr_file(const char *input_path, const char *output_path, int family) {
    return ipset_parse_cidr_file(input_path, output_path, family);
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
        
        struct stat st;
        int entry_count = 0;
        if (stat(v4_parsed, &st) == 0) {
            entry_count = st.st_size / 20;
        }
        
        if (entry_count > 1000) {
            LOG_INFO("Large GeoIP file (%d entries), using batch restore", entry_count);
        }
        
        geoip_ipset_create("cnip_temp", 4, 8192, 65536);
        geoip_ipset_restore_file("cnip_temp", v4_parsed);
        
        geoip_ipset_swap("cnip_temp", "cnip");
        LOG_INFO("IPv4 ipset upgraded to full list (%d entries)", entry_count);
        
        geoip_ipset_destroy("cnip_temp");
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
    
    if (geoip_download_url(cfg->cn_ip_url, v4_tmp, 30) != 0) {
        LOG_ERROR("Failed to download fresh IPv4 list");
        return -1;
    }
    
    geoip_parse_cidr_file(v4_tmp, v4_parsed_tmp, 4);
    
    geoip_ipset_create("cnip_temp", 4, 8192, 65536);
    geoip_ipset_restore_file("cnip_temp", v4_parsed_tmp);
    
    geoip_ipset_swap("cnip_temp", "cnip");
    LOG_INFO("IPv4 ipset swapped atomically");
    
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
        
        if (geoip_download_url(cfg->cn_ipv6_url, v6_tmp, 30) == 0) {
            geoip_parse_cidr_file(v6_tmp, v6_parsed_tmp, 6);
            geoip_ipset_create("cnip6_temp", 6, 8192, 65536);
            geoip_ipset_restore_file("cnip6_temp", v6_parsed_tmp);
            geoip_ipset_swap("cnip6_temp", "cnip6");
            LOG_INFO("IPv6 ipset swapped atomically");
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
    if (stat(v4_path, &st) != 0) {
        return 1;
    }
    
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
        if (inet_pton(AF_INET, ip, &ipv4) == 1 && prefix >= 0 && prefix <= 32) {
            return 0;
        }
    } else {
        if (inet_pton(AF_INET6, ip, &ipv6) == 1 && prefix >= 0 && prefix <= 128) {
            return 0;
        }
    }
    
    return -1;
}
