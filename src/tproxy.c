#include "tproxy.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include "boxbpf.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/wait.h>

static int g_tproxy_supported = -1;
static pthread_mutex_t g_tproxy_support_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_reject_available = -1;
static pthread_mutex_t g_reject_mutex = PTHREAD_MUTEX_INITIALIZER;

#define IPTABLES_CMD "/system/bin/iptables"
#define IP6TABLES_CMD "/system/bin/ip6tables"

static int validate_iface_name(const char *name) {
    if (!name || !*name) return -1;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-' && *p != ':') {
            return -1;
        }
    }
    return 0;
}

static int validate_ip_or_cidr(const char *str) {
    if (!str || !*str) return -1;

    struct in_addr v4;
    struct in6_addr v6;

    if (inet_pton(AF_INET, str, &v4) == 1) {
        return 0;
    }

    if (inet_pton(AF_INET6, str, &v6) == 1) {
        return 0;
    }

    char ip[128];
    int prefix = -1;
    if (sscanf(str, "%127[^/]/%d", ip, &prefix) == 2) {
        if (inet_pton(AF_INET, ip, &v4) == 1 && prefix >= 0 && prefix <= 32) {
            return 0;
        }
        if (inet_pton(AF_INET6, ip, &v6) == 1 && prefix >= 0 && prefix <= 128) {
            return 0;
        }
    }

    return -1;
}

static int exec_iptables(atp_config_t *cfg, const char *table, const char *cmd, const char *chain, const char *rule) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t %s %s %s %s", table, cmd, chain, rule ? rule : "");
        return 0;
    }

    char command[MAX_CMD_LEN];
    int n;
    if (rule) {
        n = snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                 IPTABLES_CMD, table, cmd, chain, rule);
    } else {
        n = snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                 IPTABLES_CMD, table, cmd, chain);
    }
    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command truncated");
        return -1;
    }

    int ret = exec_cmd_simple(command, CMD_TIMEOUT_SEC);
    if (ret != 0) {
        LOG_DEBUG("iptables failed: -t %s %s %s %s (ret=%d)", table, cmd, chain, rule ? rule : "", ret);
    }
    return ret;
}

static int exec_ip6tables(atp_config_t *cfg, const char *table, const char *cmd, const char *chain, const char *rule) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] ip6tables -t %s %s %s %s", table, cmd, chain, rule ? rule : "");
        return 0;
    }

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_DEBUG("ip6tables not found, skipping command");
        return -1;
    }

    char command[MAX_CMD_LEN];
    int n;
    if (rule) {
        n = snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, cmd, chain, rule);
    } else {
        n = snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, cmd, chain);
    }
    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command truncated");
        return -1;
    }

    int ret = exec_cmd_simple(command, CMD_TIMEOUT_SEC);
    if (ret != 0) {
        LOG_DEBUG("ip6tables failed: -t %s %s %s %s (ret=%d)", table, cmd, chain, rule ? rule : "", ret);
    }
    return ret;
}

int tproxy_chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (cfg->core.dry_run) {
        return 1;
    }

    char cmd[MAX_CMD_LEN];
    int n;
    char output[1024] = {0};

    if (family == 4) {
        n = snprintf(cmd, sizeof(cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                 IPTABLES_CMD, table, chain);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        n = snprintf(cmd, sizeof(cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                 IP6TABLES_CMD, table, chain);
    }
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        return 0;
    }

    int ret = exec_cmd(cmd, output, sizeof(output), CMD_TIMEOUT_SEC);
    if (ret != 0) return 0;
    if (strstr(output, "Chain") != NULL) return 1;
    return 0;
}

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (tproxy_chain_exists(cfg, family, table, chain)) {
        return 0;
    }

    if (family == 4) {
        return exec_iptables(cfg, table, "-N", chain, NULL);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-N", chain, NULL);
    }
}

int tproxy_chain_flush(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (!tproxy_chain_exists(cfg, family, table, chain)) {
        return 0;
    }

    if (family == 4) {
        return exec_iptables(cfg, table, "-F", chain, NULL);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-F", chain, NULL);
    }
}

int tproxy_chain_destroy(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (!tproxy_chain_exists(cfg, family, table, chain)) {
        return 0;
    }

    if (family == 4) {
        return exec_iptables(cfg, table, "-X", chain, NULL);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-X", chain, NULL);
    }
}

int tproxy_rule_exists(atp_config_t *cfg, int family, const char *table,
                       const char *chain, const char *rule) {
    if (cfg->core.dry_run) {
        return 0;
    }

    if (family == 4) {
        char cmd[MAX_CMD_LEN];
        int n = snprintf(cmd, sizeof(cmd), "%s -t %s -C %s %s 2>/dev/null",
                 IPTABLES_CMD, table, chain, rule);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command truncated");
            return 0;
        }
        return (exec_cmd_simple(cmd, CMD_TIMEOUT_SEC) == 0) ? 1 : 0;
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        char cmd[MAX_CMD_LEN];
        int n = snprintf(cmd, sizeof(cmd), "%s -t %s -C %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, chain, rule);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command truncated");
            return 0;
        }
        return (exec_cmd_simple(cmd, CMD_TIMEOUT_SEC) == 0) ? 1 : 0;
    }
}

int tproxy_rule_ensure_single(atp_config_t *cfg, int family, const char *table,
                              const char *chain, const char *rule) {
    while (tproxy_rule_exists(cfg, family, table, chain, rule)) {
        if (family == 4) {
            exec_iptables(cfg, table, "-D", chain, rule);
        } else {
            if (access(IP6TABLES_CMD, X_OK) != 0) break;
            exec_ip6tables(cfg, table, "-D", chain, rule);
        }
    }
    
    return tproxy_rule_add(cfg, family, table, chain, rule);
}

int tproxy_rule_ensure_single_insert(atp_config_t *cfg, int family, const char *table,
                                     const char *chain, int position, const char *rule) {
    while (tproxy_rule_exists(cfg, family, table, chain, rule)) {
        if (family == 4) {
            exec_iptables(cfg, table, "-D", chain, rule);
        } else {
            if (access(IP6TABLES_CMD, X_OK) != 0) break;
            exec_ip6tables(cfg, table, "-D", chain, rule);
        }
    }
    
    return tproxy_rule_insert(cfg, family, table, chain, position, rule);
}

int tproxy_rule_add(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (tproxy_rule_exists(cfg, family, table, chain, rule)) {
        return 0;
    }
    
    if (family == 4) {
        return exec_iptables(cfg, table, "-A", chain, rule);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-A", chain, rule);
    }
}

int tproxy_rule_del(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (!tproxy_rule_exists(cfg, family, table, chain, rule)) {
        return 0;
    }

    if (family == 4) {
        return exec_iptables(cfg, table, "-D", chain, rule);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-D", chain, rule);
    }
}

int tproxy_rule_insert(atp_config_t *cfg, int family, const char *table,
                       const char *chain, int position, const char *rule) {
    while (tproxy_rule_exists(cfg, family, table, chain, rule)) {
        if (family == 4) {
            exec_iptables(cfg, table, "-D", chain, rule);
        } else {
            if (access(IP6TABLES_CMD, X_OK) != 0) break;
            exec_ip6tables(cfg, table, "-D", chain, rule);
        }
    }
    
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] %s -t %s -I %s %d %s",
                  family == 4 ? IPTABLES_CMD : IP6TABLES_CMD,
                  table, chain, position, rule);
        return 0;
    }

    if (family == 4) {
        char cmd[MAX_CMD_LEN];
        int n = snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s 2>/dev/null",
                 IPTABLES_CMD, table, chain, position, rule);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command truncated");
            return -1;
        }
        int ret = exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
        if (ret != 0) {
            LOG_ERROR("iptables insert failed: -t %s -I %s %d %s (ret=%d)",
                      table, chain, position, rule, ret);
        }
        return ret;
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        char cmd[MAX_CMD_LEN];
        int n = snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s 2>/dev/null",
                 IP6TABLES_CMD, table, chain, position, rule);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command truncated");
            return -1;
        }
        int ret = exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
        if (ret != 0) {
            LOG_ERROR("ip6tables insert failed: -t %s -I %s %d %s (ret=%d)",
                      table, chain, position, rule, ret);
        }
        return ret;
    }
}

int tproxy_support_check(atp_config_t *cfg) {
    pthread_mutex_lock(&g_tproxy_support_mutex);
    if (g_tproxy_supported >= 0) {
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return g_tproxy_supported;
    }

    if (cfg->core.dry_run) {
        g_tproxy_supported = 1;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 1;
    }

    LOG_INFO("Running TPROXY support check...");

    char cmd[MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "%s -t mangle -N ATP_TEST 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    }
    exec_cmd_simple(cmd, 5);

    n = snprintf(cmd, sizeof(cmd), "%s -t mangle -A ATP_TEST -p tcp -j TPROXY --on-port 1536 --tproxy-mark 20 2>/dev/null",
             IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    }
    int ret = exec_cmd_simple(cmd, 5);

    n = snprintf(cmd, sizeof(cmd), "%s -t mangle -F ATP_TEST 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    }
    exec_cmd_simple(cmd, 5);
    n = snprintf(cmd, sizeof(cmd), "%s -t mangle -X ATP_TEST 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    }
    exec_cmd_simple(cmd, 5);

    g_tproxy_supported = (ret == 0) ? 1 : 0;
    LOG_INFO("TPROXY support: %s", g_tproxy_supported ? "YES" : "NO");
    pthread_mutex_unlock(&g_tproxy_support_mutex);
    return g_tproxy_supported;
}

static int tproxy_configure_rp_filter(atp_config_t *cfg) {
    DIR *dir;
    struct dirent *entry;
    char path[256];
    int success = 0;

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] Would set rp_filter=2 for all interfaces");
        return 0;
    }

    LOG_INFO("Configuring rp_filter=2 for TPROXY compatibility");

    exec_cmd_simple("echo 2 > /proc/sys/net/ipv4/conf/all/rp_filter 2>/dev/null", 2);
    exec_cmd_simple("echo 2 > /proc/sys/net/ipv4/conf/default/rp_filter 2>/dev/null", 2);

    dir = opendir("/proc/sys/net/ipv4/conf");
    if (!dir) {
        LOG_WARN("Failed to open /proc/sys/net/ipv4/conf");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        int n = snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", entry->d_name);
        if (n < 0 || n >= (int)sizeof(path)) {
            LOG_ERROR("Path truncated");
            continue;
        }
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "2\n");
            fclose(fp);
            success++;
        }
    }

    closedir(dir);
    LOG_INFO("rp_filter set to 2 for %d interfaces", success);
    return 0;
}

static int tproxy_reject_available(void) {
    pthread_mutex_lock(&g_reject_mutex);
    if (g_reject_available >= 0) {
        pthread_mutex_unlock(&g_reject_mutex);
        return g_reject_available;
    }

    char cmd[MAX_CMD_LEN];
    char output[256] = {0};

    int n = snprintf(cmd, sizeof(cmd), "%s -N ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    }
    int ret = exec_cmd_simple(cmd, 3);
    if (ret != 0) {
        n = snprintf(cmd, sizeof(cmd), "%s -X ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command truncated");
            g_reject_available = 0;
            pthread_mutex_unlock(&g_reject_mutex);
            return 0;
        }
        exec_cmd_simple(cmd, 3);
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    }

    n = snprintf(cmd, sizeof(cmd), "%s -A ATP_TEST_REJECT -j REJECT 2>&1", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    }
    ret = exec_cmd(cmd, output, sizeof(output), 3);

    n = snprintf(cmd, sizeof(cmd), "%s -F ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    }
    exec_cmd_simple(cmd, 3);
    n = snprintf(cmd, sizeof(cmd), "%s -X ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    }
    exec_cmd_simple(cmd, 3);

    if (ret != 0 || strstr(output, "No chain/target/match") != NULL) {
        g_reject_available = 0;
    } else {
        g_reject_available = 1;
    }

    pthread_mutex_unlock(&g_reject_mutex);
    return g_reject_available;
}

static void tproxy_create_standard_chains(atp_config_t *cfg, int family) {
    const char *chains_ipv4[] = {
        "ATP_PRE_0", "ATP_PRE_1",
        "ATP_OUT_0", "ATP_OUT_1",
        "ATP_DIVERT_0", "ATP_DIVERT_1",
        "ATP_PROXY_IP_0", "ATP_PROXY_IP_1",
        "ATP_BYPASS_IP_0", "ATP_BYPASS_IP_1",
        "ATP_PROXY_IFACE_0", "ATP_PROXY_IFACE_1",
        "ATP_BYPASS_IFACE_0", "ATP_BYPASS_IFACE_1",
        "ATP_DNS_PRE_0", "ATP_DNS_PRE_1",
        "ATP_DNS_OUT_0", "ATP_DNS_OUT_1",
        "ATP_APP_0", "ATP_APP_1",
        "ATP_MAC_0", "ATP_MAC_1",
        NULL
    };
    
    const char *chains_ipv6[] = {
        "ATP6_PRE_0", "ATP6_PRE_1",
        "ATP6_OUT_0", "ATP6_OUT_1",
        "ATP6_DIVERT_0", "ATP6_DIVERT_1",
        "ATP6_PROXY_IP_0", "ATP6_PROXY_IP_1",
        "ATP6_BYPASS_IP_0", "ATP6_BYPASS_IP_1",
        "ATP6_PROXY_IFACE_0", "ATP6_PROXY_IFACE_1",
        "ATP6_BYPASS_IFACE_0", "ATP6_BYPASS_IFACE_1",
        "ATP6_DNS_PRE_0", "ATP6_DNS_PRE_1",
        "ATP6_DNS_OUT_0", "ATP6_DNS_OUT_1",
        "ATP6_APP_0", "ATP6_APP_1",
        "ATP6_MAC_0", "ATP6_MAC_1",
        NULL
    };
    
    const char **chains;
    if (family == 6) {
        chains = chains_ipv6;
    } else {
        chains = chains_ipv4;
    }
    
    for (int i = 0; chains[i] != NULL; i++) {
        char chain_name[64];
        int n = snprintf(chain_name, sizeof(chain_name), "%s", chains[i]);
        if (n < 0 || n >= (int)sizeof(chain_name)) {
            LOG_ERROR("Chain name truncated");
            return;
        }
        tproxy_chain_create(cfg, family, "mangle", chain_name);
        tproxy_chain_flush(cfg, family, "mangle", chain_name);
    }
}

static void tproxy_setup_divert_chain(atp_config_t *cfg, int family, int mark) {
    char chain_name[64];
    char rule_buf[256];
    int n;

    if (family == 6) {
        n = snprintf(chain_name, sizeof(chain_name), "ATP6_DIVERT_0");
    } else {
        n = snprintf(chain_name, sizeof(chain_name), "ATP_DIVERT_0");
    }
    if (n < 0 || n >= (int)sizeof(chain_name)) {
        LOG_ERROR("Chain name truncated");
        return;
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j MARK --set-mark %d", mark);
    tproxy_rule_ensure_single(cfg, family, "mangle", chain_name, rule_buf);
    tproxy_rule_ensure_single(cfg, family, "mangle", chain_name, "-j ACCEPT");
}

static void tproxy_setup_socket_match(atp_config_t *cfg, int family, const char *divert_chain) {
    char chain_name[64];
    char rule_buf[256];
    int n;

    if (family == 6) {
        n = snprintf(chain_name, sizeof(chain_name), "ATP6_PRE_0");
    } else {
        n = snprintf(chain_name, sizeof(chain_name), "ATP_PRE_0");
    }
    if (n < 0 || n >= (int)sizeof(chain_name)) {
        LOG_ERROR("Chain name truncated");
        return;
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -m socket --transparent -j %s", divert_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", chain_name, rule_buf);
}

static void tproxy_setup_chain_jumps(atp_config_t *cfg, int family) {
    char pre_chain[64];
    char out_chain[64];
    char proxy_ip_chain[64];
    char bypass_ip_chain[64];
    char proxy_iface_chain[64];
    char bypass_iface_chain[64];
    char mac_chain[64];
    char dns_pre_chain[64];
    char dns_out_chain[64];
    char app_chain[64];
    int n;

    if (family == 6) {
        n = snprintf(pre_chain, sizeof(pre_chain), "ATP6_PRE_0");
        n += snprintf(out_chain, sizeof(out_chain), "ATP6_OUT_0");
        n += snprintf(proxy_ip_chain, sizeof(proxy_ip_chain), "ATP6_PROXY_IP_0");
        n += snprintf(bypass_ip_chain, sizeof(bypass_ip_chain), "ATP6_BYPASS_IP_0");
        n += snprintf(proxy_iface_chain, sizeof(proxy_iface_chain), "ATP6_PROXY_IFACE_0");
        n += snprintf(bypass_iface_chain, sizeof(bypass_iface_chain), "ATP6_BYPASS_IFACE_0");
        n += snprintf(mac_chain, sizeof(mac_chain), "ATP6_MAC_0");
        n += snprintf(dns_pre_chain, sizeof(dns_pre_chain), "ATP6_DNS_PRE_0");
        n += snprintf(dns_out_chain, sizeof(dns_out_chain), "ATP6_DNS_OUT_0");
        n += snprintf(app_chain, sizeof(app_chain), "ATP6_APP_0");
    } else {
        n = snprintf(pre_chain, sizeof(pre_chain), "ATP_PRE_0");
        n += snprintf(out_chain, sizeof(out_chain), "ATP_OUT_0");
        n += snprintf(proxy_ip_chain, sizeof(proxy_ip_chain), "ATP_PROXY_IP_0");
        n += snprintf(bypass_ip_chain, sizeof(bypass_ip_chain), "ATP_BYPASS_IP_0");
        n += snprintf(proxy_iface_chain, sizeof(proxy_iface_chain), "ATP_PROXY_IFACE_0");
        n += snprintf(bypass_iface_chain, sizeof(bypass_iface_chain), "ATP_BYPASS_IFACE_0");
        n += snprintf(mac_chain, sizeof(mac_chain), "ATP_MAC_0");
        n += snprintf(dns_pre_chain, sizeof(dns_pre_chain), "ATP_DNS_PRE_0");
        n += snprintf(dns_out_chain, sizeof(dns_out_chain), "ATP_DNS_OUT_0");
        n += snprintf(app_chain, sizeof(app_chain), "ATP_APP_0");
    }
    if (n < 0) {
        LOG_ERROR("snprintf failed");
        return;
    }

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", proxy_ip_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", pre_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", bypass_ip_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", pre_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", proxy_iface_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", pre_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", mac_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", pre_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", dns_pre_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", pre_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", proxy_ip_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", out_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", bypass_ip_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", out_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", bypass_iface_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", out_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", app_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", out_chain, rule_buf);
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", dns_out_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", out_chain, rule_buf);
}

static void tproxy_setup_iface_rules(atp_config_t *cfg, int family) {
    char proxy_chain[64], bypass_chain[64];
    char rule[256];
    char list_buf[512];
    char *saveptr;
    int n;

    if (family == 6) {
        n = snprintf(proxy_chain, sizeof(proxy_chain), "ATP6_PROXY_IFACE_0");
        n += snprintf(bypass_chain, sizeof(bypass_chain), "ATP6_BYPASS_IFACE_0");
    } else {
        n = snprintf(proxy_chain, sizeof(proxy_chain), "ATP_PROXY_IFACE_0");
        n += snprintf(bypass_chain, sizeof(bypass_chain), "ATP_BYPASS_IFACE_0");
    }
    if (n < 0) {
        LOG_ERROR("snprintf failed");
        return;
    }

    tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, "-i lo -j RETURN");

    if (validate_iface_name(cfg->interface.mobile_iface) == 0) {
        if (cfg->interface.proxy_mobile) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.mobile_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        } else {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.mobile_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.mobile_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
        }
    }

    int hotspot_on_wifi = (strcmp(cfg->interface.hotspot_iface, cfg->interface.wifi_iface) == 0);
    const char *hotspot_subnet = (family == 4) ? cfg->interface.hotspot_subnet_ipv4 : cfg->interface.hotspot_subnet_ipv6;

    if (hotspot_on_wifi) {
        if (cfg->interface.proxy_hotspot) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -s %s -j RETURN", cfg->interface.hotspot_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        } else {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -s %s -j ACCEPT", cfg->interface.hotspot_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        }

        if (cfg->interface.proxy_wifi) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s ! -s %s -j RETURN", cfg->interface.wifi_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        } else {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s ! -s %s -j ACCEPT", cfg->interface.wifi_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
        }
    } else {
        if (cfg->interface.proxy_wifi) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        } else {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
        }

        if (cfg->interface.proxy_hotspot) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.hotspot_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        } else {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.hotspot_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.hotspot_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
        }
    }

    if (cfg->interface.proxy_usb && validate_iface_name(cfg->interface.usb_iface) == 0) {
        SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.usb_iface);
        tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
    } else if (validate_iface_name(cfg->interface.usb_iface) == 0) {
        SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.usb_iface);
        tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.usb_iface);
        tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
    }

    if (cfg->interface.other_proxy[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", cfg->interface.other_proxy);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        }
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_iface_name(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    if (cfg->interface.other_bypass[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", cfg->interface.other_bypass);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        }
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_iface_name(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
                SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }
}

static void tproxy_setup_ip_rules(atp_config_t *cfg, int family) {
    char proxy_chain[64], bypass_chain[64];
    char rule[256];
    char list_buf[4096];
    char *saveptr;
    const char *bypass_list, *proxy_list;
    int n;

    if (family == 6) {
        n = snprintf(proxy_chain, sizeof(proxy_chain), "ATP6_PROXY_IP_0");
        n += snprintf(bypass_chain, sizeof(bypass_chain), "ATP6_BYPASS_IP_0");
        bypass_list = cfg->iplist.bypass_ipv6_list;
        proxy_list = cfg->iplist.proxy_ipv6_list;
    } else {
        n = snprintf(proxy_chain, sizeof(proxy_chain), "ATP_PROXY_IP_0");
        n += snprintf(bypass_chain, sizeof(bypass_chain), "ATP_BYPASS_IP_0");
        bypass_list = cfg->iplist.bypass_ipv4_list;
        proxy_list = cfg->iplist.proxy_ipv4_list;
    }
    if (n < 0) {
        LOG_ERROR("snprintf failed");
        return;
    }

    if (proxy_list && proxy_list[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", proxy_list);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        }
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_ip_or_cidr(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-d %s -j RETURN", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    if (bypass_list && bypass_list[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", bypass_list);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        }
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_ip_or_cidr(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-d %s -p udp ! --dport 53 -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
                SAFE_SNPRINTF(rule, sizeof(rule), "-d %s ! -p udp -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    if (cfg->filter.bypass_cn_ip) {
        if (cfg->ebpf.ready) {
            const char *pin_dir = boxbpf_pin_dir();
            const char *pin_out, *pin_pre;

            if (family == 4) {
                pin_out = "box_cidr_out4";
                pin_pre = "box_cidr_pre4";
            } else {
                pin_out = "box_cidr_out6";
                pin_pre = "box_cidr_pre6";
            }

            char bpf_rule[512];

            SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s -p udp ! --dport 53 -j ACCEPT",
                     pin_dir, pin_out);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, bpf_rule);

            SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s ! -p udp -j ACCEPT",
                     pin_dir, pin_out);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, bpf_rule);

            SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s -p udp ! --dport 53 -j ACCEPT",
                     pin_dir, pin_pre);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, bpf_rule);

            SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s ! -p udp -j ACCEPT",
                     pin_dir, pin_pre);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, bpf_rule);

            LOG_DEBUG("CNIP bypass: eBPF");
        } else {
            const char *ipset = (family == 4) ? "cnip" : "cnip6";
            SAFE_SNPRINTF(rule, sizeof(rule), "-m set --match-set %s dst -p udp ! --dport 53 -j ACCEPT", ipset);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-m set --match-set %s dst ! -p udp -j ACCEPT", ipset);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            LOG_DEBUG("CNIP bypass: ipset (%s)", ipset);
        }
    }
}

void tproxy_hook_main_chains(atp_config_t *cfg, int family) {
    char pre_chain[64];
    char out_chain[64];
    char hook_rule[128];
    int n;

    if (family == 6) {
        n = snprintf(pre_chain, sizeof(pre_chain), "ATP6_PRE_0");
        n += snprintf(out_chain, sizeof(out_chain), "ATP6_OUT_0");
    } else {
        n = snprintf(pre_chain, sizeof(pre_chain), "ATP_PRE_0");
        n += snprintf(out_chain, sizeof(out_chain), "ATP_OUT_0");
    }
    if (n < 0) {
        LOG_ERROR("snprintf failed");
        return;
    }

    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule),
             "-m owner --uid-owner %s --gid-owner %s -j RETURN",
             cfg->core.core_user, cfg->core.core_group);
    tproxy_rule_ensure_single(cfg, family, "mangle", "OUTPUT", hook_rule);

    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule), "-j %s", pre_chain);
    tproxy_rule_ensure_single_insert(cfg, family, "mangle", "PREROUTING", 1, hook_rule);

    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule), "-j %s", out_chain);
    tproxy_rule_ensure_single_insert(cfg, family, "mangle", "OUTPUT", 1, hook_rule);
}

int tproxy_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 TPROXY chains");

    tproxy_configure_rp_filter(cfg);
    tproxy_create_standard_chains(cfg, 4);
    tproxy_setup_divert_chain(cfg, 4, cfg->network.mark_value);
    tproxy_setup_socket_match(cfg, 4, "ATP_DIVERT_0");
    tproxy_setup_chain_jumps(cfg, 4);
    tproxy_setup_iface_rules(cfg, 4);
    tproxy_setup_ip_rules(cfg, 4);

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value, cfg->network.tcp_port, cfg->network.mark_value);
    tproxy_rule_ensure_single(cfg, 4, "mangle", "PREROUTING", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value, cfg->network.mark_value);
    tproxy_rule_ensure_single(cfg, 4, "mangle", "OUTPUT", rule_buf);

    tproxy_hook_main_chains(cfg, 4);

    LOG_INFO("IPv4 TPROXY setup complete");
    return 0;
}

int tproxy_setup_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping");
        return 0;
    }

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 setup skipped");
        cfg->network.proxy_ipv6 = 0;
        return 0;
    }

    LOG_INFO("Setting up IPv6 TPROXY chains");

    tproxy_create_standard_chains(cfg, 6);
    tproxy_setup_divert_chain(cfg, 6, cfg->network.mark_value6);
    tproxy_setup_socket_match(cfg, 6, "ATP6_DIVERT_0");
    tproxy_setup_chain_jumps(cfg, 6);
    tproxy_setup_iface_rules(cfg, 6);
    tproxy_setup_ip_rules(cfg, 6);

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value6, cfg->network.tcp_port, cfg->network.mark_value6);
    tproxy_rule_ensure_single(cfg, 6, "mangle", "PREROUTING", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value6, cfg->network.mark_value6);
    tproxy_rule_ensure_single(cfg, 6, "mangle", "OUTPUT", rule_buf);

    tproxy_hook_main_chains(cfg, 6);

    LOG_INFO("IPv6 TPROXY setup complete");
    return 0;
}

int tproxy_setup_redirect_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 REDIRECT chains");

    const char *table = "nat";
    char rule_buf[256];

    tproxy_chain_create(cfg, 4, table, CHAIN_REDIRECT_IPV4);
    tproxy_chain_flush(cfg, 4, table, CHAIN_REDIRECT_IPV4);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->network.tcp_port);
    tproxy_rule_ensure_single(cfg, 4, table, CHAIN_REDIRECT_IPV4, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", CHAIN_REDIRECT_IPV4);
    tproxy_rule_ensure_single_insert(cfg, 4, table, "PREROUTING", 1, rule_buf);
    tproxy_rule_ensure_single_insert(cfg, 4, table, "OUTPUT", 1, rule_buf);

    LOG_INFO("IPv4 REDIRECT setup complete");
    return 0;
}

int tproxy_setup_redirect_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) return 0;

    LOG_INFO("Setting up IPv6 REDIRECT chains");

    const char *table = "nat";
    char rule_buf[256];

    tproxy_chain_create(cfg, 6, table, CHAIN_REDIRECT_IPV6);
    tproxy_chain_flush(cfg, 6, table, CHAIN_REDIRECT_IPV6);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->network.tcp_port);
    tproxy_rule_ensure_single(cfg, 6, table, CHAIN_REDIRECT_IPV6, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", CHAIN_REDIRECT_IPV6);
    tproxy_rule_ensure_single_insert(cfg, 6, table, "PREROUTING", 1, rule_buf);
    tproxy_rule_ensure_single_insert(cfg, 6, table, "OUTPUT", 1, rule_buf);

    LOG_INFO("IPv6 REDIRECT setup complete");
    return 0;
}

int tproxy_setup_enhance_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up ENHANCE mode for IPv4");

    const char *table_mangle = "mangle";
    const char *table_nat = "nat";
    char rule_buf[256];

    tproxy_chain_create(cfg, 4, table_nat, "ATP_REDIRECT_TCP");
    tproxy_chain_flush(cfg, 4, table_nat, "ATP_REDIRECT_TCP");

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d",
             cfg->network.redirect_tcp_port);
    tproxy_rule_ensure_single(cfg, 4, table_nat, "ATP_REDIRECT_TCP", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j ATP_REDIRECT_TCP");
    tproxy_rule_ensure_single_insert(cfg, 4, table_nat, "PREROUTING", 1, rule_buf);
    tproxy_rule_ensure_single_insert(cfg, 4, table_nat, "OUTPUT", 1, rule_buf);

    tproxy_chain_create(cfg, 4, table_mangle, "ATP_UDP_TPROXY");
    tproxy_chain_flush(cfg, 4, table_mangle, "ATP_UDP_TPROXY");

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.udp_port, cfg->network.mark_value);
    tproxy_rule_ensure_single(cfg, 4, table_mangle, "ATP_UDP_TPROXY", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j ATP_UDP_TPROXY");
    tproxy_rule_ensure_single_insert(cfg, 4, table_mangle, "PREROUTING", 1, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value);
    tproxy_rule_ensure_single_insert(cfg, 4, table_mangle, "OUTPUT", 1, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value, cfg->network.redirect_tcp_port);
    tproxy_rule_ensure_single(cfg, 4, table_nat, "PREROUTING", rule_buf);
    tproxy_rule_ensure_single(cfg, 4, table_nat, "OUTPUT", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core.core_user, cfg->core.core_group);
    tproxy_rule_ensure_single_insert(cfg, 4, table_nat, "OUTPUT", 1, rule_buf);

    LOG_INFO("IPv4 ENHANCE mode setup complete");
    return 0;
}

int tproxy_setup_enhance_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping ENHANCE mode");
        return 0;
    }

    LOG_INFO("Setting up ENHANCE mode for IPv6");

    const char *table_mangle = "mangle";
    const char *table_nat = "nat";
    char rule_buf[256];

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 ENHANCE mode skipped");
        return 0;
    }

    tproxy_chain_create(cfg, 6, table_nat, "ATP6_REDIRECT_TCP");
    tproxy_chain_flush(cfg, 6, table_nat, "ATP6_REDIRECT_TCP");

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d",
             cfg->network.redirect_tcp_port);
    tproxy_rule_ensure_single(cfg, 6, table_nat, "ATP6_REDIRECT_TCP", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j ATP6_REDIRECT_TCP");
    tproxy_rule_ensure_single_insert(cfg, 6, table_nat, "PREROUTING", 1, rule_buf);
    tproxy_rule_ensure_single_insert(cfg, 6, table_nat, "OUTPUT", 1, rule_buf);

    tproxy_chain_create(cfg, 6, table_mangle, "ATP6_UDP_TPROXY");
    tproxy_chain_flush(cfg, 6, table_mangle, "ATP6_UDP_TPROXY");

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.udp_port, cfg->network.mark_value6);
    tproxy_rule_ensure_single(cfg, 6, table_mangle, "ATP6_UDP_TPROXY", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j ATP6_UDP_TPROXY");
    tproxy_rule_ensure_single_insert(cfg, 6, table_mangle, "PREROUTING", 1, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value6);
    tproxy_rule_ensure_single_insert(cfg, 6, table_mangle, "OUTPUT", 1, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value6, cfg->network.redirect_tcp_port);
    tproxy_rule_ensure_single(cfg, 6, table_nat, "PREROUTING", rule_buf);
    tproxy_rule_ensure_single(cfg, 6, table_nat, "OUTPUT", rule_buf);

    LOG_INFO("IPv6 ENHANCE mode setup complete");
    return 0;
}

static void tproxy_cleanup_owner_rule(atp_config_t *cfg, int family) {
    char hook_rule[128];
    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule),
             "-m owner --uid-owner %s --gid-owner %s -j RETURN",
             cfg->core.core_user, cfg->core.core_group);
    
    while (tproxy_rule_exists(cfg, family, "mangle", "OUTPUT", hook_rule)) {
        tproxy_rule_del(cfg, family, "mangle", "OUTPUT", hook_rule);
    }
}

int tproxy_cleanup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 TPROXY chains");

    tproxy_cleanup_owner_rule(cfg, 4);

    while (tproxy_rule_exists(cfg, 4, "mangle", "PREROUTING", "-j ATP_PRE_0")) {
        tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j ATP_PRE_0");
    }
    while (tproxy_rule_exists(cfg, 4, "mangle", "OUTPUT", "-j ATP_OUT_0")) {
        tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j ATP_OUT_0");
    }

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value, cfg->network.tcp_port, cfg->network.mark_value);
    while (tproxy_rule_exists(cfg, 4, "mangle", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", rule_buf);
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value, cfg->network.mark_value);
    while (tproxy_rule_exists(cfg, 4, "mangle", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", rule_buf);
    }

    const char *chains[] = {
        "ATP_PRE_0", "ATP_PRE_1",
        "ATP_OUT_0", "ATP_OUT_1",
        "ATP_DIVERT_0", "ATP_DIVERT_1",
        "ATP_PROXY_IP_0", "ATP_PROXY_IP_1",
        "ATP_BYPASS_IP_0", "ATP_BYPASS_IP_1",
        "ATP_PROXY_IFACE_0", "ATP_PROXY_IFACE_1",
        "ATP_BYPASS_IFACE_0", "ATP_BYPASS_IFACE_1",
        "ATP_DNS_PRE_0", "ATP_DNS_PRE_1",
        "ATP_DNS_OUT_0", "ATP_DNS_OUT_1",
        "ATP_APP_0", "ATP_APP_1",
        "ATP_MAC_0", "ATP_MAC_1",
        NULL
    };

    for (int i = 0; chains[i] != NULL; i++) {
        tproxy_chain_flush(cfg, 4, "mangle", chains[i]);
        tproxy_chain_destroy(cfg, 4, "mangle", chains[i]);
    }

    LOG_INFO("IPv4 TPROXY cleanup complete");
    return 0;
}

int tproxy_cleanup_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) return 0;

    LOG_INFO("Cleaning up IPv6 TPROXY chains");

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_DEBUG("ip6tables not found, skipping IPv6 cleanup");
        return 0;
    }

    tproxy_cleanup_owner_rule(cfg, 6);

    while (tproxy_rule_exists(cfg, 6, "mangle", "PREROUTING", "-j ATP6_PRE_0")) {
        tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-j ATP6_PRE_0");
    }
    while (tproxy_rule_exists(cfg, 6, "mangle", "OUTPUT", "-j ATP6_OUT_0")) {
        tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", "-j ATP6_OUT_0");
    }

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value6, cfg->network.tcp_port, cfg->network.mark_value6);
    while (tproxy_rule_exists(cfg, 6, "mangle", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", rule_buf);
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value6, cfg->network.mark_value6);
    while (tproxy_rule_exists(cfg, 6, "mangle", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", rule_buf);
    }

    const char *chains[] = {
        "ATP6_PRE_0", "ATP6_PRE_1",
        "ATP6_OUT_0", "ATP6_OUT_1",
        "ATP6_DIVERT_0", "ATP6_DIVERT_1",
        "ATP6_PROXY_IP_0", "ATP6_PROXY_IP_1",
        "ATP6_BYPASS_IP_0", "ATP6_BYPASS_IP_1",
        "ATP6_PROXY_IFACE_0", "ATP6_PROXY_IFACE_1",
        "ATP6_BYPASS_IFACE_0", "ATP6_BYPASS_IFACE_1",
        "ATP6_DNS_PRE_0", "ATP6_DNS_PRE_1",
        "ATP6_DNS_OUT_0", "ATP6_DNS_OUT_1",
        "ATP6_APP_0", "ATP6_APP_1",
        "ATP6_MAC_0", "ATP6_MAC_1",
        NULL
    };

    for (int i = 0; chains[i] != NULL; i++) {
        tproxy_chain_flush(cfg, 6, "mangle", chains[i]);
        tproxy_chain_destroy(cfg, 6, "mangle", chains[i]);
    }

    LOG_INFO("IPv6 TPROXY cleanup complete");
    return 0;
}

int tproxy_cleanup_redirect_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 REDIRECT chains");
    
    char rule_buf[256];
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", CHAIN_REDIRECT_IPV4);
    while (tproxy_rule_exists(cfg, 4, "nat", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 4, "nat", "PREROUTING", rule_buf);
    }
    while (tproxy_rule_exists(cfg, 4, "nat", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 4, "nat", "OUTPUT", rule_buf);
    }
    
    tproxy_chain_flush(cfg, 4, "nat", CHAIN_REDIRECT_IPV4);
    tproxy_chain_destroy(cfg, 4, "nat", CHAIN_REDIRECT_IPV4);
    
    LOG_INFO("IPv4 REDIRECT cleanup complete");
    return 0;
}

int tproxy_cleanup_redirect_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) return 0;
    
    LOG_INFO("Cleaning up IPv6 REDIRECT chains");
    
    char rule_buf[256];
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", CHAIN_REDIRECT_IPV6);
    while (tproxy_rule_exists(cfg, 6, "nat", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 6, "nat", "PREROUTING", rule_buf);
    }
    while (tproxy_rule_exists(cfg, 6, "nat", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 6, "nat", "OUTPUT", rule_buf);
    }
    
    tproxy_chain_flush(cfg, 6, "nat", CHAIN_REDIRECT_IPV6);
    tproxy_chain_destroy(cfg, 6, "nat", CHAIN_REDIRECT_IPV6);
    
    LOG_INFO("IPv6 REDIRECT cleanup complete");
    return 0;
}

int tproxy_cleanup_enhance_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 ENHANCE mode");

    char rule_buf[256];

    while (tproxy_rule_exists(cfg, 4, "nat", "PREROUTING", "-p tcp -j ATP_REDIRECT_TCP")) {
        tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-p tcp -j ATP_REDIRECT_TCP");
    }
    while (tproxy_rule_exists(cfg, 4, "nat", "OUTPUT", "-p tcp -j ATP_REDIRECT_TCP")) {
        tproxy_rule_del(cfg, 4, "nat", "OUTPUT", "-p tcp -j ATP_REDIRECT_TCP");
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value, cfg->network.redirect_tcp_port);
    while (tproxy_rule_exists(cfg, 4, "nat", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 4, "nat", "PREROUTING", rule_buf);
    }
    while (tproxy_rule_exists(cfg, 4, "nat", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 4, "nat", "OUTPUT", rule_buf);
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core.core_user, cfg->core.core_group);
    while (tproxy_rule_exists(cfg, 4, "nat", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 4, "nat", "OUTPUT", rule_buf);
    }

    tproxy_chain_flush(cfg, 4, "nat", "ATP_REDIRECT_TCP");
    tproxy_chain_destroy(cfg, 4, "nat", "ATP_REDIRECT_TCP");

    while (tproxy_rule_exists(cfg, 4, "mangle", "PREROUTING", "-p udp -j ATP_UDP_TPROXY")) {
        tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-p udp -j ATP_UDP_TPROXY");
    }
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value);
    while (tproxy_rule_exists(cfg, 4, "mangle", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", rule_buf);
    }
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_UDP_TPROXY");
    tproxy_chain_destroy(cfg, 4, "mangle", "ATP_UDP_TPROXY");

    LOG_INFO("IPv4 ENHANCE mode cleanup complete");
    return 0;
}

int tproxy_cleanup_enhance_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) return 0;

    LOG_INFO("Cleaning up IPv6 ENHANCE mode");

    char rule_buf[256];

    while (tproxy_rule_exists(cfg, 6, "nat", "PREROUTING", "-p tcp -j ATP6_REDIRECT_TCP")) {
        tproxy_rule_del(cfg, 6, "nat", "PREROUTING", "-p tcp -j ATP6_REDIRECT_TCP");
    }
    while (tproxy_rule_exists(cfg, 6, "nat", "OUTPUT", "-p tcp -j ATP6_REDIRECT_TCP")) {
        tproxy_rule_del(cfg, 6, "nat", "OUTPUT", "-p tcp -j ATP6_REDIRECT_TCP");
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value6, cfg->network.redirect_tcp_port);
    while (tproxy_rule_exists(cfg, 6, "nat", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 6, "nat", "PREROUTING", rule_buf);
    }
    while (tproxy_rule_exists(cfg, 6, "nat", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 6, "nat", "OUTPUT", rule_buf);
    }

    if (access(IP6TABLES_CMD, X_OK) == 0) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
                 cfg->core.core_user, cfg->core.core_group);
        while (tproxy_rule_exists(cfg, 6, "nat", "OUTPUT", rule_buf)) {
            tproxy_rule_del(cfg, 6, "nat", "OUTPUT", rule_buf);
        }
    }

    tproxy_chain_flush(cfg, 6, "nat", "ATP6_REDIRECT_TCP");
    tproxy_chain_destroy(cfg, 6, "nat", "ATP6_REDIRECT_TCP");

    while (tproxy_rule_exists(cfg, 6, "mangle", "PREROUTING", "-p udp -j ATP6_UDP_TPROXY")) {
        tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-p udp -j ATP6_UDP_TPROXY");
    }
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value6);
    while (tproxy_rule_exists(cfg, 6, "mangle", "OUTPUT", rule_buf)) {
        tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", rule_buf);
    }
    tproxy_chain_flush(cfg, 6, "mangle", "ATP6_UDP_TPROXY");
    tproxy_chain_destroy(cfg, 6, "mangle", "ATP6_UDP_TPROXY");

    LOG_INFO("IPv6 ENHANCE mode cleanup complete");
    return 0;
}

int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Cleaning up XFRM bypass chains");

    while (tproxy_rule_exists(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS")) {
        tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    }
    while (tproxy_rule_exists(cfg, 4, "mangle", "OUTPUT", "-j XFRM_BYPASS")) {
        tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j XFRM_BYPASS");
    }
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_destroy(cfg, 4, "mangle", "XFRM_BYPASS");

    while (tproxy_rule_exists(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT")) {
        tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT");
    }
    while (tproxy_rule_exists(cfg, 4, "nat", "OUTPUT", "-j XFRM_BYPASS_NAT")) {
        tproxy_rule_del(cfg, 4, "nat", "OUTPUT", "-j XFRM_BYPASS_NAT");
    }
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_destroy(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass chains cleaned up");
    return 0;
}

int tproxy_cleanup_orphan_chains(atp_config_t *cfg) {
    LOG_INFO("Cleaning up orphan ATP chains");
    
    const char *tables[] = {"mangle", "nat", "filter"};
    char cmd[MAX_CMD_LEN];
    
    for (int family = 4; family <= 6; family += 2) {
        for (int t = 0; t < 3; t++) {
            int retry_count = 5;
            while (retry_count-- > 0) {
                int found = 0;
                
                if (family == 4) {
                    int n = snprintf(cmd, sizeof(cmd),
                            "%s -t %s -S 2>/dev/null | grep -E '^-N (ATP_|ATP6_|XFRM_BYPASS)' | awk '{print $2}'",
                            IPTABLES_CMD, tables[t]);
                    if (n < 0 || n >= (int)sizeof(cmd)) {
                        LOG_ERROR("Command truncated");
                        continue;
                    }
                } else {
                    if (access(IP6TABLES_CMD, X_OK) != 0) continue;
                    int n = snprintf(cmd, sizeof(cmd),
                            "%s -t %s -S 2>/dev/null | grep -E '^-N (ATP_|ATP6_|XFRM_BYPASS)' | awk '{print $2}'",
                            IP6TABLES_CMD, tables[t]);
                    if (n < 0 || n >= (int)sizeof(cmd)) {
                        LOG_ERROR("Command truncated");
                        continue;
                    }
                }
                
                FILE *fp = popen(cmd, "r");
                if (!fp) continue;
                
                char chain_name[256];
                while (fgets(chain_name, sizeof(chain_name), fp)) {
                    chain_name[strcspn(chain_name, "\n")] = 0;
                    found = 1;
                    
                    int retry = 20;
                    while (tproxy_chain_exists(cfg, family, tables[t], chain_name) && retry-- > 0) {
                        if (family == 4) {
                            int n = snprintf(cmd, sizeof(cmd),
                                    "%s -t %s -F %s 2>/dev/null",
                                    IPTABLES_CMD, tables[t], chain_name);
                            if (n < 0 || n >= (int)sizeof(cmd)) {
                                LOG_ERROR("Command truncated");
                                continue;
                            }
                        } else {
                            int n = snprintf(cmd, sizeof(cmd),
                                    "%s -t %s -F %s 2>/dev/null",
                                    IP6TABLES_CMD, tables[t], chain_name);
                            if (n < 0 || n >= (int)sizeof(cmd)) {
                                LOG_ERROR("Command truncated");
                                continue;
                            }
                        }
                        exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
                        
                        if (family == 4) {
                            int n = snprintf(cmd, sizeof(cmd),
                                    "%s -t %s -X %s 2>/dev/null",
                                    IPTABLES_CMD, tables[t], chain_name);
                            if (n < 0 || n >= (int)sizeof(cmd)) {
                                LOG_ERROR("Command truncated");
                                continue;
                            }
                        } else {
                            int n = snprintf(cmd, sizeof(cmd),
                                    "%s -t %s -X %s 2>/dev/null",
                                    IP6TABLES_CMD, tables[t], chain_name);
                            if (n < 0 || n >= (int)sizeof(cmd)) {
                                LOG_ERROR("Command truncated");
                                continue;
                            }
                        }
                        exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
                    }
                    
                    if (retry <= 0) {
                        LOG_WARN("Failed to destroy chain %s after 20 attempts", chain_name);
                    } else {
                        LOG_DEBUG("Removed orphan chain: %s from table %s", chain_name, tables[t]);
                    }
                }
                pclose(fp);
                
                if (!found) break;
            }
        }
    }
    
    return 0;
}

int tproxy_cleanup_all(atp_config_t *cfg) {
    LOG_INFO("Starting full cleanup of all ATP rules");
    
    tproxy_dns_hijack_cleanup(cfg, 4);
    if (cfg->network.proxy_ipv6) {
        tproxy_dns_hijack_cleanup(cfg, 6);
    }
    
    tproxy_block_loopback(cfg, 0);
    tproxy_block_quic(cfg, 0);
    tproxy_cleanup_xfrm_bypass(cfg);
    
    switch (cfg->network.proxy_mode) {
        case MODE_ENHANCE:
            tproxy_cleanup_enhance_ipv4(cfg);
            if (cfg->network.proxy_ipv6) tproxy_cleanup_enhance_ipv6(cfg);
            break;
        case MODE_TPROXY:
            tproxy_cleanup_ipv4(cfg);
            if (cfg->network.proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
        case MODE_REDIRECT:
            tproxy_cleanup_redirect_ipv4(cfg);
            if (cfg->network.proxy_ipv6) tproxy_cleanup_redirect_ipv6(cfg);
            break;
        default:
            tproxy_cleanup_ipv4(cfg);
            if (cfg->network.proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
    }
    
    tproxy_cleanup_orphan_chains(cfg);
    
    LOG_INFO("Full cleanup completed");
    return 0;
}

int tproxy_restart(atp_config_t *cfg) {
    LOG_INFO("Starting TPROXY restart (stop + start)");
    
    tproxy_cleanup_all(cfg);
    
    if (!tproxy_support_check(cfg)) {
        LOG_ERROR("TPROXY not supported on this kernel");
        return -1;
    }
    
    int ret = 0;
    switch (cfg->network.proxy_mode) {
        case MODE_TPROXY:
            ret = tproxy_setup_ipv4(cfg);
            if (ret == 0 && cfg->network.proxy_ipv6) {
                ret = tproxy_setup_ipv6(cfg);
            }
            break;
        case MODE_REDIRECT:
            ret = tproxy_setup_redirect_ipv4(cfg);
            if (ret == 0 && cfg->network.proxy_ipv6) {
                ret = tproxy_setup_redirect_ipv6(cfg);
            }
            break;
        case MODE_ENHANCE:
            ret = tproxy_setup_enhance_ipv4(cfg);
            if (ret == 0 && cfg->network.proxy_ipv6) {
                ret = tproxy_setup_enhance_ipv6(cfg);
            }
            break;
    }
    
    if (ret != 0) {
        LOG_ERROR("Failed to setup TPROXY rules");
        return ret;
    }
    
    if (cfg->network.dns_hijack != DNS_HIJACK_OFF) {
        tproxy_dns_hijack_setup(cfg, 4, cfg->network.dns_hijack);
        if (cfg->network.proxy_ipv6) {
            tproxy_dns_hijack_setup(cfg, 6, cfg->network.dns_hijack);
        }
    }
    
    if (cfg->core.block_quic) {
        tproxy_block_quic(cfg, 1);
    }
    
    if (cfg->network.loopback_protect) {
        tproxy_block_loopback(cfg, 1);
    }
    
    tproxy_prevent_loop(cfg);
    tproxy_xfrm_bypass(cfg);
    
    LOG_INFO("TPROXY restart completed successfully");
    return 0;
}

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode) {
    if (cfg->network.dns_hijack == DNS_HIJACK_OFF) return 0;
    if (mode == DNS_HIJACK_OFF) return 0;

    LOG_INFO("Setting up DNS hijack for IPv%d (mode=%d)", family, mode);

    char rule_buf[128];
    char dns_pre_chain[64];
    char dns_out_chain[64];
    int n;
    int mark = (family == 6) ? cfg->network.mark_value6 : cfg->network.mark_value;

    if (family == 6) {
        n = snprintf(dns_pre_chain, sizeof(dns_pre_chain), "ATP6_DNS_PRE_0");
        n += snprintf(dns_out_chain, sizeof(dns_out_chain), "ATP6_DNS_OUT_0");
    } else {
        n = snprintf(dns_pre_chain, sizeof(dns_pre_chain), "ATP_DNS_PRE_0");
        n += snprintf(dns_out_chain, sizeof(dns_out_chain), "ATP_DNS_OUT_0");
    }
    if (n < 0) {
        LOG_ERROR("snprintf failed");
        return -1;
    }

    if (mode == DNS_HIJACK_TPROXY) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
                 cfg->network.dns_port, mark);
        tproxy_rule_ensure_single(cfg, family, "mangle", dns_pre_chain, rule_buf);
        tproxy_rule_ensure_single(cfg, family, "mangle", dns_out_chain, rule_buf);
    } else if (mode == DNS_HIJACK_REDIRECT) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j REDIRECT --to-ports %d",
                 cfg->network.dns_port);
        tproxy_rule_ensure_single(cfg, family, "mangle", dns_pre_chain, rule_buf);
        tproxy_rule_ensure_single(cfg, family, "mangle", dns_out_chain, rule_buf);
    }

    return 0;
}

int tproxy_dns_hijack_cleanup(atp_config_t *cfg, int family) {
    char rule_buf[128];
    char dns_pre_chain[64];
    char dns_out_chain[64];
    int n;
    int mark = (family == 6) ? cfg->network.mark_value6 : cfg->network.mark_value;

    if (family == 6) {
        n = snprintf(dns_pre_chain, sizeof(dns_pre_chain), "ATP6_DNS_PRE_0");
        n += snprintf(dns_out_chain, sizeof(dns_out_chain), "ATP6_DNS_OUT_0");
    } else {
        n = snprintf(dns_pre_chain, sizeof(dns_pre_chain), "ATP_DNS_PRE_0");
        n += snprintf(dns_out_chain, sizeof(dns_out_chain), "ATP_DNS_OUT_0");
    }
    if (n < 0) {
        LOG_ERROR("snprintf failed");
        return -1;
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.dns_port, mark);
    while (tproxy_rule_exists(cfg, family, "mangle", dns_pre_chain, rule_buf)) {
        tproxy_rule_del(cfg, family, "mangle", dns_pre_chain, rule_buf);
    }
    while (tproxy_rule_exists(cfg, family, "mangle", dns_out_chain, rule_buf)) {
        tproxy_rule_del(cfg, family, "mangle", dns_out_chain, rule_buf);
    }

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp --dport 53 -j REDIRECT --to-ports %d",
             cfg->network.dns_port);
    while (tproxy_rule_exists(cfg, family, "mangle", dns_pre_chain, rule_buf)) {
        tproxy_rule_del(cfg, family, "mangle", dns_pre_chain, rule_buf);
    }
    while (tproxy_rule_exists(cfg, family, "mangle", dns_out_chain, rule_buf)) {
        tproxy_rule_del(cfg, family, "mangle", dns_out_chain, rule_buf);
    }

    return 0;
}

static int tproxy_reject_or_drop(atp_config_t *cfg, int family, const char *chain, const char *rule) {
    int reject_avail = tproxy_reject_available();

    char modified_rule[256];
    if (reject_avail) {
        SAFE_SNPRINTF(modified_rule, sizeof(modified_rule), "%s -j REJECT", rule);
    } else {
        SAFE_SNPRINTF(modified_rule, sizeof(modified_rule), "%s -j DROP", rule);
    }

    return tproxy_rule_ensure_single(cfg, family, "filter", chain, modified_rule);
}

int tproxy_block_quic(atp_config_t *cfg, int enable) {
    if (enable) {
        LOG_INFO("Enabling QUIC blocking");

        tproxy_chain_create(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");

        if (cfg->ebpf.ready) {
            const char *pin_dir = boxbpf_pin_dir();
            char bpf_rule[512];
            SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/box_cidr_out4 -p udp --dport 443 -j REJECT",
                     pin_dir);
            tproxy_rule_ensure_single(cfg, 4, "filter", "ATP_QUIC_0", bpf_rule);
            LOG_DEBUG("QUIC blocking: eBPF");
        } else {
            tproxy_reject_or_drop(cfg, 4, "ATP_QUIC_0", "-p udp --dport 443");
            LOG_DEBUG("QUIC blocking: ipset fallback");
        }

        tproxy_rule_ensure_single(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_ensure_single(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_ensure_single(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            tproxy_chain_create(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");

            if (cfg->ebpf.ready) {
                const char *pin_dir = boxbpf_pin_dir();
                char bpf_rule[512];
                SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                         "-m bpf --object-pinned %s/box_cidr_out6 -p udp --dport 443 -j REJECT",
                         pin_dir);
                tproxy_rule_ensure_single(cfg, 6, "filter", "ATP6_QUIC_0", bpf_rule);
            } else {
                tproxy_reject_or_drop(cfg, 6, "ATP6_QUIC_0", "-p udp --dport 443");
            }

            tproxy_rule_ensure_single(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            tproxy_rule_ensure_single(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            tproxy_rule_ensure_single(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
        }
    } else {
        LOG_INFO("Disabling QUIC blocking");

        while (tproxy_rule_exists(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0")) {
            tproxy_rule_del(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        }
        while (tproxy_rule_exists(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0")) {
            tproxy_rule_del(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        }
        while (tproxy_rule_exists(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0")) {
            tproxy_rule_del(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");
        }
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_destroy(cfg, 4, "filter", "ATP_QUIC_0");

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            while (tproxy_rule_exists(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0")) {
                tproxy_rule_del(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            }
            while (tproxy_rule_exists(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0")) {
                tproxy_rule_del(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            }
            while (tproxy_rule_exists(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0")) {
                tproxy_rule_del(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
            }
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_destroy(cfg, 6, "filter", "ATP6_QUIC_0");
        }
    }

    return 0;
}

int tproxy_block_loopback(atp_config_t *cfg, int enable) {
    char rule_buf[256];

    if (enable) {
        LOG_INFO("Enabling loopback protection");
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->network.tcp_port);
        tproxy_rule_ensure_single(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->network.tcp_port);
            tproxy_rule_ensure_single(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    } else {
        LOG_INFO("Disabling loopback protection");
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->network.tcp_port);
        while (tproxy_rule_exists(cfg, 4, "filter", "OUTPUT", rule_buf)) {
            tproxy_rule_del(cfg, 4, "filter", "OUTPUT", rule_buf);
        }

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->network.tcp_port);
            while (tproxy_rule_exists(cfg, 6, "filter", "OUTPUT", rule_buf)) {
                tproxy_rule_del(cfg, 6, "filter", "OUTPUT", rule_buf);
            }
        }
    }

    return 0;
}

int tproxy_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Setting up XFRM bypass for VPN traffic");

    tproxy_chain_create(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");

    tproxy_rule_ensure_single(cfg, 4, "mangle", "XFRM_BYPASS", "-p esp -j RETURN");
    tproxy_rule_ensure_single(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_ensure_single(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 500 -j RETURN");

    tproxy_rule_ensure_single_insert(cfg, 4, "mangle", "PREROUTING", 1, "-j XFRM_BYPASS");
    tproxy_rule_ensure_single_insert(cfg, 4, "mangle", "OUTPUT", 1, "-j XFRM_BYPASS");

    tproxy_chain_create(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    tproxy_rule_ensure_single(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p esp -j RETURN");
    tproxy_rule_ensure_single(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_ensure_single(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p udp --dport 500 -j RETURN");

    tproxy_rule_ensure_single_insert(cfg, 4, "nat", "PREROUTING", 1, "-j XFRM_BYPASS_NAT");
    tproxy_rule_ensure_single_insert(cfg, 4, "nat", "OUTPUT", 1, "-j XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass configured");
    return 0;
}

int tproxy_prevent_loop(atp_config_t *cfg) {
    char rule_buf[64];
    
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->network.mark_value);
    
    while (tproxy_rule_exists(cfg, 4, "mangle", "PREROUTING", rule_buf)) {
        tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", rule_buf);
    }
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, rule_buf);
    
    if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->network.mark_value6);
        
        while (tproxy_rule_exists(cfg, 6, "mangle", "PREROUTING", rule_buf)) {
            tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", rule_buf);
        }
        tproxy_rule_insert(cfg, 6, "mangle", "PREROUTING", 1, rule_buf);
    }
    
    return 0;
}

int tproxy_sound_bypass(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}