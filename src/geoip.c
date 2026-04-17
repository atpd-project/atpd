#include "geoip.h"
#include "logger.h"
#include "utils.h"
#include "atp.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <time.h>

struct curl_memory {
    char *data;
    size_t size;
};

static size_t curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ATPd/" ATP_VERSION);
    
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
    } else {
        LOG_WARN("Failed to download IPv4 list, using cached if available");
    }
    
    if (cfg->proxy_ipv6) {
        char v6_path[PATH_MAX];
        char v6_tmp[PATH_MAX];
        snprintf(v6_path, sizeof(v6_path), "%s/%s", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_tmp, sizeof(v6_tmp), "%s/%s.tmp", rules_dir, cfg->cn_ipv6_file);
        
        LOG_INFO("Downloading China IPv6 list");
        if (geoip_download_url(cfg->cn_ipv6_url, v6_tmp, 30) == 0) {
            rename(v6_tmp, v6_path);
            LOG_INFO("IPv6 list downloaded successfully");
        } else {
            LOG_WARN("Failed to download IPv6 list");
        }
    }
    
    return 0;
}

int geoip_ipset_create(const char *name, int family, int hashsize, int maxelem) {
    char cmd[MAX_CMD_LEN];
    const char *family_str = (family == 4) ? "inet" : "inet6";
    
    snprintf(cmd, sizeof(cmd), 
             "ipset create %s hash:net family %s hashsize %d maxelem %d 2>/dev/null",
             name, family_str, hashsize, maxelem);
    
    return exec_cmd_simple(cmd, 5);
}

int geoip_ipset_destroy(const char *name) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset destroy %s 2>/dev/null", name);
    return exec_cmd_simple(cmd, 5);
}

int geoip_ipset_swap(const char *from, const char *to) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset swap %s %s 2>/dev/null", from, to);
    return exec_cmd_simple(cmd, 5);
}

int geoip_ipset_exists(const char *name) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset list %s 2>/dev/null | head -1", name);
    
    char output[256];
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0]) {
        return 1;
    }
    return 0;
}

int geoip_ipset_flush(const char *name) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "ipset flush %s 2>/dev/null", name);
    return exec_cmd_simple(cmd, 5);
}

int geoip_ipset_restore_file(const char *name, const char *filename) {
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), 
             "awk '!/^[[:space:]]*#/ && NF > 0 {printf \"add %s %s\\n\", $0}' %s | ipset restore -exist 2>/dev/null",
             name, name, filename);
    return exec_cmd_simple(cmd, 30);
}

int geoip_parse_cidr_file(const char *input_path, const char *output_path, int family) {
    FILE *fin = fopen(input_path, "r");
    if (!fin) return -1;
    
    FILE *fout = fopen(output_path, "w");
    if (!fout) {
        fclose(fin);
        return -1;
    }
    
    char line[256];
    struct in_addr ipv4;
    struct in6_addr ipv6;
    
    while (fgets(line, sizeof(line), fin)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        
        char ip[128];
        int prefix = 0;
        
        if (sscanf(line, "%127[^/]/%d", ip, &prefix) == 2) {
            int valid = 0;
            if (family == 4) {
                if (inet_pton(AF_INET, ip, &ipv4) == 1 && prefix >= 0 && prefix <= 32) {
                    valid = 1;
                }
            } else {
                if (inet_pton(AF_INET6, ip, &ipv6) == 1 && prefix >= 0 && prefix <= 128) {
                    valid = 1;
                }
            }
            
            if (valid) {
                fprintf(fout, "%s\n", line);
            }
        }
    }
    
    fclose(fin);
    fclose(fout);
    return 0;
}

int geoip_setup_ipset(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) {
        LOG_DEBUG("CN IP bypass disabled, skipping ipset setup");
        return 0;
    }
    
    char rules_dir[PATH_MAX];
    char v4_path[PATH_MAX];
    char v4_parsed[PATH_MAX];
    
    snprintf(rules_dir, sizeof(rules_dir), "%s/rules", cfg->data_dir);
    snprintf(v4_path, sizeof(v4_path), "%s/%s", rules_dir, cfg->cn_ip_file);
    snprintf(v4_parsed, sizeof(v4_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ip_file);
    
    if (!file_exists(v4_path)) {
        LOG_WARN("IPv4 list not found, skipping ipset");
        return -1;
    }
    
    geoip_parse_cidr_file(v4_path, v4_parsed, 4);
    
    geoip_ipset_destroy("cnip");
    geoip_ipset_create("cnip", 4, 8192, 65536);
    geoip_ipset_restore_file("cnip", v4_parsed);
    
    LOG_INFO("IPv4 ipset 'cnip' loaded");
    
    if (cfg->proxy_ipv6) {
        char v6_path[PATH_MAX];
        char v6_parsed[PATH_MAX];
        snprintf(v6_path, sizeof(v6_path), "%s/%s", rules_dir, cfg->cn_ipv6_file);
        snprintf(v6_parsed, sizeof(v6_parsed), "%s/%s.parsed", rules_dir, cfg->cn_ipv6_file);
        
        if (file_exists(v6_path)) {
            geoip_parse_cidr_file(v6_path, v6_parsed, 6);
            geoip_ipset_destroy("cnip6");
            geoip_ipset_create("cnip6", 6, 8192, 65536);
            geoip_ipset_restore_file("cnip6", v6_parsed);
            LOG_INFO("IPv6 ipset 'cnip6' loaded");
        } else {
            LOG_WARN("IPv6 list not found");
        }
    }
    
    return 0;
}

int geoip_cleanup_ipset(atp_config_t *cfg) {
    if (!cfg->bypass_cn_ip) return 0;
    
    geoip_ipset_destroy("cnip");
    geoip_ipset_destroy("cnip6");
    
    LOG_INFO("GeoIP ipsets destroyed");
    return 0;
}

int geoip_atomic_update(atp_config_t *cfg) {
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
    
    if (geoip_ipset_exists("cnip")) {
        geoip_ipset_swap("cnip_temp", "cnip");
        LOG_INFO("IPv4 ipset swapped atomically");
    } else {
        geoip_ipset_swap("cnip_temp", "cnip");
        LOG_INFO("IPv4 ipset created");
    }
    
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
            
            if (geoip_ipset_exists("cnip6")) {
                geoip_ipset_swap("cnip6_temp", "cnip6");
                LOG_INFO("IPv6 ipset swapped atomically");
            } else {
                geoip_ipset_swap("cnip6_temp", "cnip6");
                LOG_INFO("IPv6 ipset created");
            }
            
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