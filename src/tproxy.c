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

static int g_tproxy_supported = -1;
static pthread_mutex_t g_tproxy_support_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_reject_available = -1;
static pthread_mutex_t g_reject_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_restore_available_v4 = -1;
static int g_restore_available_v6 = -1;
static pthread_mutex_t g_restore_mutex = PTHREAD_MUTEX_INITIALIZER;

#define IPTABLES_CMD "/system/bin/iptables"
#define IP6TABLES_CMD "/system/bin/ip6tables"
#define IPTABLES_RESTORE_CMD "/system/bin/iptables-restore"
#define IP6TABLES_RESTORE_CMD "/system/bin/ip6tables-restore"

static int validate_iface_name(const char *name) {
    if (!name || !*name) return -1;
    for (const char *p = name; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != ':') {
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
    int prefix;
    if (sscanf(str, "%127[^/]/%d", ip, &prefix) == 2) {
        if (inet_pton(AF_INET, ip, &v4) == 1 && prefix <= 32) return 0;
        if (inet_pton(AF_INET6, ip, &v6) == 1 && prefix <= 128) return 0;
    }

    return -1;
}

static int exec_iptables(atp_config_t *cfg, const char *table, const char *cmd, const char *chain, const char *rule) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] iptables -t %s %s %s %s", table, cmd, chain, rule ? rule : "");
        return 0;
    }

    char command[MAX_CMD_LEN];
    if (rule) {
        snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                 IPTABLES_CMD, table, cmd, chain, rule);
    } else {
        snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                 IPTABLES_CMD, table, cmd, chain);
    }

    int ret = exec_cmd_simple(command, CMD_TIMEOUT_SEC);
    if (ret != 0) {
        LOG_ERROR("iptables failed: -t %s %s %s %s (ret=%d)", table, cmd, chain, rule ? rule : "", ret);
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
    if (rule) {
        snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, cmd, chain, rule);
    } else {
        snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                 IP6TABLES_CMD, table, cmd, chain);
    }

    int ret = exec_cmd_simple(command, CMD_TIMEOUT_SEC);
    if (ret != 0) {
        LOG_ERROR("ip6tables failed: -t %s %s %s %s (ret=%d)", table, cmd, chain, rule ? rule : "", ret);
    }
    return ret;
}

static int exec_iptables_restore(const char *rules, int family) {
    const char *cmd;
    int *cache;

    if (family == 4) {
        cmd = IPTABLES_RESTORE_CMD;
        cache = &g_restore_available_v4;
    } else {
        cmd = IP6TABLES_RESTORE_CMD;
        cache = &g_restore_available_v6;
    }

    pthread_mutex_lock(&g_restore_mutex);
    if (*cache == -1) {
        *cache = (access(cmd, X_OK) == 0) ? 1 : 0;
    }
    pthread_mutex_unlock(&g_restore_mutex);

    if (*cache != 1) {
        return -1;
    }

    FILE *fp = popen(cmd, "w");
    if (!fp) {
        LOG_ERROR("Failed to popen %s", cmd);
        return -1;
    }
    fwrite(rules, 1, strlen(rules), fp);
    int ret = pclose(fp);
    if (ret != 0) {
        LOG_ERROR("%s batch restore failed (ret=%d)", cmd, ret);
    }
    return ret;
}

int tproxy_chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] Check chain %s in table %s", chain, table);
        return 1;
    }

    char cmd[MAX_CMD_LEN];
    if (family == 4) {
        snprintf(cmd, sizeof(cmd), "%s -t %s -S %s 2>/dev/null", IPTABLES_CMD, table, chain);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        snprintf(cmd, sizeof(cmd), "%s -t %s -S %s 2>/dev/null", IP6TABLES_CMD, table, chain);
    }

    return (exec_cmd_simple(cmd, CMD_TIMEOUT_SEC) == 0) ? 1 : 0;
}

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (tproxy_chain_exists(cfg, family, table, chain)) {
        LOG_DEBUG("Chain %s already exists in table %s", chain, table);
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

int tproxy_rule_add(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-A", chain, rule);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-A", chain, rule);
    }
}

int tproxy_rule_del(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (family == 4) {
        return exec_iptables(cfg, table, "-D", chain, rule);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        return exec_ip6tables(cfg, table, "-D", chain, rule);
    }
}

int tproxy_rule_insert(atp_config_t *cfg, int family, const char *table,
                       const char *chain, int position, const char *rule) {
    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] %s -t %s -I %s %d %s",
                  family == 4 ? IPTABLES_CMD : IP6TABLES_CMD,
                  table, chain, position, rule);
        return 0;
    }

    if (family == 4) {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s 2>/dev/null",
                 IPTABLES_CMD, table, chain, position, rule);
        int ret = exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
        if (ret != 0) {
            LOG_ERROR("iptables insert failed: -t %s -I %s %d %s (ret=%d)",
                      table, chain, position, rule, ret);
        }
        return ret;
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s 2>/dev/null",
                 IP6TABLES_CMD, table, chain, position, rule);
        int ret = exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
        if (ret != 0) {
            LOG_ERROR("ip6tables insert failed: -t %s -I %s %d %s (ret=%d)",
                      table, chain, position, rule, ret);
        }
        return ret;
    }
}

int tproxy_atomic_switch(atp_config_t *cfg, int family, const char *table,
                         const char *hook, const char *chain0, const char *chain1) {
    char rule_buf[256];
    int ret;

    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain0);
    ret = tproxy_rule_del(cfg, family, table, hook, rule_buf);
    if (ret != 0) {
        LOG_ERROR("Atomic switch: failed to remove old chain %s (ret=%d), aborting", chain0, ret);
        return ret;
    }

    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain1);
    ret = tproxy_rule_insert(cfg, family, table, hook, 1, rule_buf);
    if (ret != 0) {
        LOG_ERROR("Atomic switch: failed to insert new chain %s (ret=%d)", chain1, ret);
        snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain0);
        tproxy_rule_insert(cfg, family, table, hook, 1, rule_buf);
    }
    return ret;
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

    LOG_INFO("Running TPROXY support check (cached for lifetime)...");

    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "%s -t mangle -N ATP_TEST 2>/dev/null", IPTABLES_CMD);
    exec_cmd_simple(cmd, 5);

    snprintf(cmd, sizeof(cmd), "%s -t mangle -A ATP_TEST -p tcp -j TPROXY --on-port 1536 --tproxy-mark 20 2>/dev/null",
             IPTABLES_CMD);
    int ret = exec_cmd_simple(cmd, 5);

    snprintf(cmd, sizeof(cmd), "%s -t mangle -F ATP_TEST 2>/dev/null", IPTABLES_CMD);
    exec_cmd_simple(cmd, 5);
    snprintf(cmd, sizeof(cmd), "%s -t mangle -X ATP_TEST 2>/dev/null", IPTABLES_CMD);
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

        snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", entry->d_name);
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "2\n");
            fclose(fp);
            success++;
        } else {
            LOG_DEBUG("Failed to set rp_filter for %s", entry->d_name);
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

    snprintf(cmd, sizeof(cmd), "%s -N ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    int ret = exec_cmd_simple(cmd, 3);
    if (ret != 0) {
        snprintf(cmd, sizeof(cmd), "%s -X ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
        exec_cmd_simple(cmd, 3);
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "%s -A ATP_TEST_REJECT -j REJECT 2>&1", IPTABLES_CMD);
    ret = exec_cmd(cmd, output, sizeof(output), 3);

    snprintf(cmd, sizeof(cmd), "%s -F ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    exec_cmd_simple(cmd, 3);
    snprintf(cmd, sizeof(cmd), "%s -X ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    exec_cmd_simple(cmd, 3);

    if (ret != 0 || strstr(output, "No chain/target/match") != NULL) {
        g_reject_available = 0;
    } else {
        g_reject_available = 1;
    }

    pthread_mutex_unlock(&g_reject_mutex);
    return g_reject_available;
}

static void tproxy_create_standard_chains(atp_config_t *cfg, int family, const char *suffix) {
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
        char chain_name[64];
        if (suffix && suffix[0]) {
            snprintf(chain_name, sizeof(chain_name), "%s%s", chains[i], suffix);
        } else {
            snprintf(chain_name, sizeof(chain_name), "%s", chains[i]);
        }
        tproxy_chain_create(cfg, family, "mangle", chain_name);
        tproxy_chain_flush(cfg, family, "mangle", chain_name);
    }
}

static void tproxy_setup_divert_chain(atp_config_t *cfg, int family, const char *suffix, int mark) {
    char chain_name[64];
    char rule_buf[256];

    if (suffix && suffix[0]) {
        snprintf(chain_name, sizeof(chain_name), "ATP_DIVERT_0%s", suffix);
    } else {
        snprintf(chain_name, sizeof(chain_name), "ATP_DIVERT_0");
    }

    snprintf(rule_buf, sizeof(rule_buf), "-j MARK --set-mark %d", mark);
    tproxy_rule_add(cfg, family, "mangle", chain_name, rule_buf);
    tproxy_rule_add(cfg, family, "mangle", chain_name, "-j ACCEPT");
}

static void tproxy_setup_socket_match(atp_config_t *cfg, int family, const char *suffix, const char *divert_chain) {
    char chain_name[64];
    char rule_buf[256];

    if (suffix && suffix[0]) {
        snprintf(chain_name, sizeof(chain_name), "ATP_PRE_0%s", suffix);
    } else {
        snprintf(chain_name, sizeof(chain_name), "ATP_PRE_0");
    }
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m socket --transparent -j %s", divert_chain);

    tproxy_rule_add(cfg, family, "mangle", chain_name, rule_buf);
}

static void tproxy_setup_chain_jumps(atp_config_t *cfg, int family, const char *suffix, int has_conntrack) {
    char pre_chain[64];
    char out_chain[64];
    (void)has_conntrack;

    if (suffix && suffix[0]) {
        snprintf(pre_chain, sizeof(pre_chain), "ATP_PRE_0%s", suffix);
        snprintf(out_chain, sizeof(out_chain), "ATP_OUT_0%s", suffix);
    } else {
        snprintf(pre_chain, sizeof(pre_chain), "ATP_PRE_0");
        snprintf(out_chain, sizeof(out_chain), "ATP_OUT_0");
    }

    tproxy_rule_add(cfg, family, "mangle", pre_chain, "-j ATP_PROXY_IP_0");
    tproxy_rule_add(cfg, family, "mangle", pre_chain, "-j ATP_BYPASS_IP_0");
    tproxy_rule_add(cfg, family, "mangle", pre_chain, "-j ATP_PROXY_IFACE_0");
    tproxy_rule_add(cfg, family, "mangle", pre_chain, "-j ATP_MAC_0");
    tproxy_rule_add(cfg, family, "mangle", pre_chain, "-j ATP_DNS_PRE_0");

    tproxy_rule_add(cfg, family, "mangle", out_chain, "-j ATP_PROXY_IP_0");
    tproxy_rule_add(cfg, family, "mangle", out_chain, "-j ATP_BYPASS_IP_0");
    tproxy_rule_add(cfg, family, "mangle", out_chain, "-j ATP_BYPASS_IFACE_0");
    tproxy_rule_add(cfg, family, "mangle", out_chain, "-j ATP_APP_0");
    tproxy_rule_add(cfg, family, "mangle", out_chain, "-j ATP_DNS_OUT_0");
}

static void tproxy_setup_iface_rules(atp_config_t *cfg, int family, const char *suffix) {
    char proxy_chain[64], bypass_chain[64];
    char rule[256];
    char list_buf[512];
    char *saveptr;

    if (suffix && suffix[0]) {
        snprintf(proxy_chain, sizeof(proxy_chain), "ATP_PROXY_IFACE_0%s", suffix);
        snprintf(bypass_chain, sizeof(bypass_chain), "ATP_BYPASS_IFACE_0%s", suffix);
    } else {
        snprintf(proxy_chain, sizeof(proxy_chain), "ATP_PROXY_IFACE_0");
        snprintf(bypass_chain, sizeof(bypass_chain), "ATP_BYPASS_IFACE_0");
    }

    tproxy_rule_add(cfg, family, "mangle", proxy_chain, "-i lo -j RETURN");

    if (validate_iface_name(cfg->interface.mobile_iface) == 0) {
        if (cfg->interface.proxy_mobile) {
            snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.mobile_iface);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.mobile_iface);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.mobile_iface);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
        }
    }

    int hotspot_on_wifi = (strcmp(cfg->interface.hotspot_iface, cfg->interface.wifi_iface) == 0);
    const char *hotspot_subnet = (family == 4) ? cfg->interface.hotspot_subnet_ipv4 : cfg->interface.hotspot_subnet_ipv6;

    if (hotspot_on_wifi) {
        if (cfg->interface.proxy_hotspot) {
            snprintf(rule, sizeof(rule), "-i %s -s %s -j RETURN", cfg->interface.hotspot_iface, hotspot_subnet);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -s %s -j ACCEPT", cfg->interface.hotspot_iface, hotspot_subnet);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        }

        if (cfg->interface.proxy_wifi) {
            snprintf(rule, sizeof(rule), "-i %s ! -s %s -j RETURN", cfg->interface.wifi_iface, hotspot_subnet);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s ! -s %s -j ACCEPT", cfg->interface.wifi_iface, hotspot_subnet);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
        }
    } else {
        if (cfg->interface.proxy_wifi) {
            snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.wifi_iface);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
        }

        if (cfg->interface.proxy_hotspot) {
            snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.hotspot_iface);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        } else {
            snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.hotspot_iface);
            tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
            snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.hotspot_iface);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
        }
    }

    if (cfg->interface.proxy_usb && validate_iface_name(cfg->interface.usb_iface) == 0) {
        snprintf(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.usb_iface);
        tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
    } else if (validate_iface_name(cfg->interface.usb_iface) == 0) {
        snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.usb_iface);
        tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
        snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.usb_iface);
        tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
    }

    if (cfg->interface.other_proxy[0]) {
        snprintf(list_buf, sizeof(list_buf), "%s", cfg->interface.other_proxy);
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_iface_name(token) == 0) {
                snprintf(rule, sizeof(rule), "-i %s -j RETURN", token);
                tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    if (cfg->interface.other_bypass[0]) {
        snprintf(list_buf, sizeof(list_buf), "%s", cfg->interface.other_bypass);
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_iface_name(token) == 0) {
                snprintf(rule, sizeof(rule), "-i %s -j ACCEPT", token);
                tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
                snprintf(rule, sizeof(rule), "-o %s -j ACCEPT", token);
                tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }
}

static void tproxy_setup_ip_rules(atp_config_t *cfg, int family, const char *suffix) {
    char proxy_chain[64], bypass_chain[64];
    char rule[256];
    char list_buf[4096];
    char *saveptr;
    const char *bypass_list, *proxy_list;

    if (suffix && suffix[0]) {
        snprintf(proxy_chain, sizeof(proxy_chain), "ATP_PROXY_IP_0%s", suffix);
        snprintf(bypass_chain, sizeof(bypass_chain), "ATP_BYPASS_IP_0%s", suffix);
        bypass_list = (family == 4) ? cfg->iplist.bypass_ipv4_list : cfg->iplist.bypass_ipv6_list;
        proxy_list = (family == 4) ? cfg->iplist.proxy_ipv4_list : cfg->iplist.proxy_ipv6_list;
    } else {
        snprintf(proxy_chain, sizeof(proxy_chain), "ATP_PROXY_IP_0");
        snprintf(bypass_chain, sizeof(bypass_chain), "ATP_BYPASS_IP_0");
        bypass_list = cfg->iplist.bypass_ipv4_list;
        proxy_list = cfg->iplist.proxy_ipv4_list;
    }

    if (proxy_list && proxy_list[0]) {
        snprintf(list_buf, sizeof(list_buf), "%s", proxy_list);
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_ip_or_cidr(token) == 0) {
                snprintf(rule, sizeof(rule), "-d %s -j RETURN", token);
                tproxy_rule_add(cfg, family, "mangle", proxy_chain, rule);
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    if (bypass_list && bypass_list[0]) {
        snprintf(list_buf, sizeof(list_buf), "%s", bypass_list);
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_ip_or_cidr(token) == 0) {
                snprintf(rule, sizeof(rule), "-d %s -p udp ! --dport 53 -j ACCEPT", token);
                tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
                snprintf(rule, sizeof(rule), "-d %s ! -p udp -j ACCEPT", token);
                tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
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

            snprintf(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s -p udp ! --dport 53 -j ACCEPT",
                     pin_dir, pin_out);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, bpf_rule);

            snprintf(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s ! -p udp -j ACCEPT",
                     pin_dir, pin_out);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, bpf_rule);

            snprintf(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s -p udp ! --dport 53 -j ACCEPT",
                     pin_dir, pin_pre);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, bpf_rule);

            snprintf(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/%s ! -p udp -j ACCEPT",
                     pin_dir, pin_pre);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, bpf_rule);

            LOG_DEBUG("CNIP bypass: eBPF (pin=%s)", pin_dir);
        } else {
            const char *ipset = (family == 4) ? "cnip" : "cnip6";
            snprintf(rule, sizeof(rule), "-m set --match-set %s dst -p udp ! --dport 53 -j ACCEPT", ipset);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
            snprintf(rule, sizeof(rule), "-m set --match-set %s dst ! -p udp -j ACCEPT", ipset);
            tproxy_rule_add(cfg, family, "mangle", bypass_chain, rule);
            LOG_DEBUG("CNIP bypass: ipset (%s)", ipset);
        }
    }
}

void tproxy_hook_main_chains(atp_config_t *cfg, int family, const char *suffix) {
    char pre_chain[64];
    char out_chain[64];
    char hook_rule[128];

    if (suffix && suffix[0]) {
        snprintf(pre_chain, sizeof(pre_chain), "ATP_PRE_0%s", suffix);
        snprintf(out_chain, sizeof(out_chain), "ATP_OUT_0%s", suffix);
    } else {
        snprintf(pre_chain, sizeof(pre_chain), "ATP_PRE_0");
        snprintf(out_chain, sizeof(out_chain), "ATP_OUT_0");
    }

    snprintf(hook_rule, sizeof(hook_rule),
             "-m owner --uid-owner %s --gid-owner %s -j RETURN",
             cfg->core.core_user, cfg->core.core_group);
    tproxy_rule_del(cfg, family, "mangle", "OUTPUT", hook_rule);
    tproxy_rule_insert(cfg, family, "mangle", "OUTPUT", 1, hook_rule);

    snprintf(hook_rule, sizeof(hook_rule), "-j %s", pre_chain);
    tproxy_rule_del(cfg, family, "mangle", "PREROUTING", hook_rule);
    tproxy_rule_insert(cfg, family, "mangle", "PREROUTING", 1, hook_rule);

    snprintf(hook_rule, sizeof(hook_rule), "-j %s", out_chain);
    tproxy_rule_del(cfg, family, "mangle", "OUTPUT", hook_rule);
    tproxy_rule_insert(cfg, family, "mangle", "OUTPUT", 1, hook_rule);
}

#define APPEND_RULE(buf, offset, maxlen, ...) \
do { \
    int ret = snprintf(buf + offset, maxlen - offset, __VA_ARGS__); \
    if (ret < 0) { \
        LOG_ERROR("snprintf failed in batch build"); \
        return -1; \
    } \
    if (ret >= (int)(maxlen - offset)) { \
        LOG_ERROR("Batch rules overflow at offset %zu", offset); \
        return -1; \
    } \
    offset += ret; \
} while (0)

int tproxy_setup_ipv4_batch(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 TPROXY chains (batch mode)");

    tproxy_configure_rp_filter(cfg);

    char rules[16384];
    size_t offset = 0;
    const size_t maxlen = sizeof(rules);

    APPEND_RULE(rules, offset, maxlen,
        "*mangle\n"
        ":PREROUTING ACCEPT [0:0]\n"
        ":OUTPUT ACCEPT [0:0]\n"
        ":ATP_PRE_0 - [0:0]\n"
        ":ATP_PRE_1 - [0:0]\n"
        ":ATP_OUT_0 - [0:0]\n"
        ":ATP_OUT_1 - [0:0]\n"
        ":ATP_DIVERT_0 - [0:0]\n"
        ":ATP_DIVERT_1 - [0:0]\n"
        ":ATP_PROXY_IP_0 - [0:0]\n"
        ":ATP_PROXY_IP_1 - [0:0]\n"
        ":ATP_BYPASS_IP_0 - [0:0]\n"
        ":ATP_BYPASS_IP_1 - [0:0]\n"
        ":ATP_PROXY_IFACE_0 - [0:0]\n"
        ":ATP_PROXY_IFACE_1 - [0:0]\n"
        ":ATP_BYPASS_IFACE_0 - [0:0]\n"
        ":ATP_BYPASS_IFACE_1 - [0:0]\n"
        ":ATP_DNS_PRE_0 - [0:0]\n"
        ":ATP_DNS_PRE_1 - [0:0]\n"
        ":ATP_DNS_OUT_0 - [0:0]\n"
        ":ATP_DNS_OUT_1 - [0:0]\n"
        ":ATP_APP_0 - [0:0]\n"
        ":ATP_APP_1 - [0:0]\n"
        ":ATP_MAC_0 - [0:0]\n"
        ":ATP_MAC_1 - [0:0]\n"
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A ATP_DIVERT_0 -j MARK --set-mark %d\n"
        "-A ATP_DIVERT_0 -j ACCEPT\n",
        cfg->network.mark_value
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A ATP_PRE_0 -p tcp -m socket --transparent -j ATP_DIVERT_0\n"
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A ATP_PRE_0 -j ATP_PROXY_IP_0\n"
        "-A ATP_PRE_0 -j ATP_BYPASS_IP_0\n"
        "-A ATP_PRE_0 -j ATP_PROXY_IFACE_0\n"
        "-A ATP_PRE_0 -j ATP_MAC_0\n"
        "-A ATP_PRE_0 -j ATP_DNS_PRE_0\n"
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A ATP_OUT_0 -j ATP_PROXY_IP_0\n"
        "-A ATP_OUT_0 -j ATP_BYPASS_IP_0\n"
        "-A ATP_OUT_0 -j ATP_BYPASS_IFACE_0\n"
        "-A ATP_OUT_0 -j ATP_APP_0\n"
        "-A ATP_OUT_0 -j ATP_DNS_OUT_0\n"
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A OUTPUT -m owner --uid-owner %s --gid-owner %s -j RETURN\n",
        cfg->core.core_user, cfg->core.core_group
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A PREROUTING -m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d\n"
        "-A OUTPUT -m connmark --mark %d/0xff -j MARK --set-mark %d\n",
        cfg->network.mark_value, cfg->network.tcp_port, cfg->network.mark_value,
        cfg->network.mark_value, cfg->network.mark_value
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A PREROUTING -j ATP_PRE_0\n"
        "-A OUTPUT -j ATP_OUT_0\n"
        "COMMIT\n"
    );

    int ret = exec_iptables_restore(rules, 4);
    if (ret != 0) {
        LOG_ERROR("Batch restore failed (ret=%d), falling back to sequential mode", ret);
        return tproxy_setup_ipv4(cfg);
    }
    return 0;
}

int tproxy_setup_ipv6_batch(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping");
        return 0;
    }

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 setup skipped");
        cfg->network.proxy_ipv6 = 0;
        return 0;
    }

    LOG_INFO("Setting up IPv6 TPROXY chains (batch mode)");

    char rules[16384];
    size_t offset = 0;
    const size_t maxlen = sizeof(rules);

    APPEND_RULE(rules, offset, maxlen,
        "*mangle\n"
        ":PREROUTING ACCEPT [0:0]\n"
        ":OUTPUT ACCEPT [0:0]\n"
        ":ATP6_PRE_0 - [0:0]\n"
        ":ATP6_PRE_1 - [0:0]\n"
        ":ATP6_OUT_0 - [0:0]\n"
        ":ATP6_OUT_1 - [0:0]\n"
        ":ATP6_DIVERT_0 - [0:0]\n"
        ":ATP6_DIVERT_1 - [0:0]\n"
        ":ATP6_PROXY_IP_0 - [0:0]\n"
        ":ATP6_PROXY_IP_1 - [0:0]\n"
        ":ATP6_BYPASS_IP_0 - [0:0]\n"
        ":ATP6_BYPASS_IP_1 - [0:0]\n"
        ":ATP6_PROXY_IFACE_0 - [0:0]\n"
        ":ATP6_PROXY_IFACE_1 - [0:0]\n"
        ":ATP6_BYPASS_IFACE_0 - [0:0]\n"
        ":ATP6_BYPASS_IFACE_1 - [0:0]\n"
        ":ATP6_DNS_PRE_0 - [0:0]\n"
        ":ATP6_DNS_PRE_1 - [0:0]\n"
        ":ATP6_DNS_OUT_0 - [0:0]\n"
        ":ATP6_DNS_OUT_1 - [0:0]\n"
        ":ATP6_APP_0 - [0:0]\n"
        ":ATP6_APP_1 - [0:0]\n"
        ":ATP6_MAC_0 - [0:0]\n"
        ":ATP6_MAC_1 - [0:0]\n"
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A ATP6_DIVERT_0 -j MARK --set-mark %d\n"
        "-A ATP6_DIVERT_0 -j ACCEPT\n",
        cfg->network.mark_value6
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A ATP6_PRE_0 -p tcp -m socket --transparent -j ATP6_DIVERT_0\n"
        "-A ATP6_PRE_0 -j ATP6_PROXY_IP_0\n"
        "-A ATP6_PRE_0 -j ATP6_BYPASS_IP_0\n"
        "-A ATP6_PRE_0 -j ATP6_PROXY_IFACE_0\n"
        "-A ATP6_PRE_0 -j ATP6_MAC_0\n"
        "-A ATP6_PRE_0 -j ATP6_DNS_PRE_0\n"
        "-A ATP6_OUT_0 -j ATP6_PROXY_IP_0\n"
        "-A ATP6_OUT_0 -j ATP6_BYPASS_IP_0\n"
        "-A ATP6_OUT_0 -j ATP6_BYPASS_IFACE_0\n"
        "-A ATP6_OUT_0 -j ATP6_APP_0\n"
        "-A ATP6_OUT_0 -j ATP6_DNS_OUT_0\n"
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A OUTPUT -m owner --uid-owner %s --gid-owner %s -j RETURN\n",
        cfg->core.core_user, cfg->core.core_group
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A PREROUTING -m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d\n"
        "-A OUTPUT -m connmark --mark %d/0xff -j MARK --set-mark %d\n",
        cfg->network.mark_value6, cfg->network.tcp_port, cfg->network.mark_value6,
        cfg->network.mark_value6, cfg->network.mark_value6
    );

    APPEND_RULE(rules, offset, maxlen,
        "-A PREROUTING -j ATP6_PRE_0\n"
        "-A OUTPUT -j ATP6_OUT_0\n"
        "COMMIT\n"
    );

    int ret = exec_iptables_restore(rules, 6);
    if (ret != 0) {
        LOG_ERROR("Batch restore failed (ret=%d), falling back to sequential mode", ret);
        return tproxy_setup_ipv6(cfg);
    }
    return 0;
}

int tproxy_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 TPROXY chains");

    tproxy_configure_rp_filter(cfg);
    tproxy_create_standard_chains(cfg, 4, "");
    tproxy_setup_divert_chain(cfg, 4, "", cfg->network.mark_value);
    tproxy_setup_socket_match(cfg, 4, "", "ATP_DIVERT_0");
    tproxy_setup_chain_jumps(cfg, 4, "", 1);
    tproxy_setup_iface_rules(cfg, 4, "");
    tproxy_setup_ip_rules(cfg, 4, "");

    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value, cfg->network.tcp_port, cfg->network.mark_value);
    tproxy_rule_add(cfg, 4, "mangle", "PREROUTING", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value, cfg->network.mark_value);
    tproxy_rule_add(cfg, 4, "mangle", "OUTPUT", rule_buf);

    tproxy_hook_main_chains(cfg, 4, "");

    LOG_INFO("IPv4 TPROXY setup complete with DIVERT optimization");
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

    tproxy_create_standard_chains(cfg, 6, "6");
    tproxy_setup_divert_chain(cfg, 6, "6", cfg->network.mark_value6);
    tproxy_setup_socket_match(cfg, 6, "6", "ATP6_DIVERT_0");
    tproxy_setup_chain_jumps(cfg, 6, "6", 1);
    tproxy_setup_iface_rules(cfg, 6, "6");
    tproxy_setup_ip_rules(cfg, 6, "6");

    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value6, cfg->network.tcp_port, cfg->network.mark_value6);
    tproxy_rule_add(cfg, 6, "mangle", "PREROUTING", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value6, cfg->network.mark_value6);
    tproxy_rule_add(cfg, 6, "mangle", "OUTPUT", rule_buf);

    tproxy_hook_main_chains(cfg, 6, "6");

    LOG_INFO("IPv6 TPROXY setup complete");
    return 0;
}

int tproxy_setup_redirect_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 REDIRECT chains");

    const char *table = "nat";
    char chain_name[64];
    char rule_buf[256];

    snprintf(chain_name, sizeof(chain_name), "ATP_REDIRECT");
    tproxy_chain_create(cfg, 4, table, chain_name);
    tproxy_chain_flush(cfg, 4, table, chain_name);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->network.tcp_port);
    tproxy_rule_add(cfg, 4, table, chain_name, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain_name);
    tproxy_rule_insert(cfg, 4, table, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 4, table, "OUTPUT", 1, rule_buf);

    LOG_INFO("IPv4 REDIRECT setup complete");
    return 0;
}

int tproxy_setup_redirect_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) return 0;

    LOG_INFO("Setting up IPv6 REDIRECT chains");

    const char *table = "nat";
    char chain_name[64];
    char rule_buf[256];

    snprintf(chain_name, sizeof(chain_name), "ATP6_REDIRECT");
    tproxy_chain_create(cfg, 6, table, chain_name);
    tproxy_chain_flush(cfg, 6, table, chain_name);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->network.tcp_port);
    tproxy_rule_add(cfg, 6, table, chain_name, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain_name);
    tproxy_rule_insert(cfg, 6, table, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 6, table, "OUTPUT", 1, rule_buf);

    LOG_INFO("IPv6 REDIRECT setup complete");
    return 0;
}

int tproxy_setup_enhance_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up ENHANCE mode for IPv4 (TCP=REDIRECT:%d, UDP=TPROXY:%d)",
             cfg->network.redirect_tcp_port, cfg->network.udp_port);

    const char *table_mangle = "mangle";
    const char *table_nat = "nat";
    char rule_buf[256];
    char chain_name[64];

    LOG_INFO("Setting up TCP REDIRECT chain (port %d)", cfg->network.redirect_tcp_port);

    snprintf(chain_name, sizeof(chain_name), "ATP_REDIRECT_TCP");
    tproxy_chain_create(cfg, 4, table_nat, chain_name);
    tproxy_chain_flush(cfg, 4, table_nat, chain_name);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d",
             cfg->network.redirect_tcp_port);
    tproxy_rule_add(cfg, 4, table_nat, chain_name, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j %s", chain_name);
    tproxy_rule_insert(cfg, 4, table_nat, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 4, table_nat, "OUTPUT", 1, rule_buf);

    LOG_INFO("Setting up UDP TPROXY chain (port %d, mark %d)",
             cfg->network.udp_port, cfg->network.mark_value);

    snprintf(chain_name, sizeof(chain_name), "ATP_UDP_TPROXY");
    tproxy_chain_create(cfg, 4, table_mangle, chain_name);
    tproxy_chain_flush(cfg, 4, table_mangle, chain_name);

    snprintf(rule_buf, sizeof(rule_buf),
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.udp_port, cfg->network.mark_value);
    tproxy_rule_add(cfg, 4, table_mangle, chain_name, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j %s", chain_name);
    tproxy_rule_insert(cfg, 4, table_mangle, "PREROUTING", 1, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value);
    tproxy_rule_insert(cfg, 4, table_mangle, "OUTPUT", 1, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value, cfg->network.redirect_tcp_port);
    tproxy_rule_add(cfg, 4, table_nat, "PREROUTING", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value, cfg->network.redirect_tcp_port);
    tproxy_rule_add(cfg, 4, table_nat, "OUTPUT", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf),
             "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core.core_user, cfg->core.core_group);
    tproxy_rule_insert(cfg, 4, table_nat, "OUTPUT", 1, rule_buf);

    LOG_INFO("IPv4 ENHANCE mode setup complete");
    return 0;
}

int tproxy_setup_enhance_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping ENHANCE mode");
        return 0;
    }

    LOG_INFO("Setting up ENHANCE mode for IPv6 (TCP=REDIRECT:%d, UDP=TPROXY:%d)",
             cfg->network.redirect_tcp_port, cfg->network.udp_port);

    const char *table_mangle = "mangle";
    const char *table_nat = "nat";
    char rule_buf[256];
    char chain_name[64];

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 ENHANCE mode skipped");
        return 0;
    }

    LOG_INFO("Setting up IPv6 TCP REDIRECT chain (port %d)", cfg->network.redirect_tcp_port);

    snprintf(chain_name, sizeof(chain_name), "ATP6_REDIRECT_TCP");
    tproxy_chain_create(cfg, 6, table_nat, chain_name);
    tproxy_chain_flush(cfg, 6, table_nat, chain_name);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d",
             cfg->network.redirect_tcp_port);
    tproxy_rule_add(cfg, 6, table_nat, chain_name, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j %s", chain_name);
    tproxy_rule_insert(cfg, 6, table_nat, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 6, table_nat, "OUTPUT", 1, rule_buf);

    LOG_INFO("Setting up IPv6 UDP TPROXY chain (port %d, mark %d)",
             cfg->network.udp_port, cfg->network.mark_value6);

    snprintf(chain_name, sizeof(chain_name), "ATP6_UDP_TPROXY");
    tproxy_chain_create(cfg, 6, table_mangle, chain_name);
    tproxy_chain_flush(cfg, 6, table_mangle, chain_name);

    snprintf(rule_buf, sizeof(rule_buf),
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.udp_port, cfg->network.mark_value6);
    tproxy_rule_add(cfg, 6, table_mangle, chain_name, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j %s", chain_name);
    tproxy_rule_insert(cfg, 6, table_mangle, "PREROUTING", 1, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value6);
    tproxy_rule_insert(cfg, 6, table_mangle, "OUTPUT", 1, rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value6, cfg->network.redirect_tcp_port);
    tproxy_rule_add(cfg, 6, table_nat, "PREROUTING", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value6, cfg->network.redirect_tcp_port);
    tproxy_rule_add(cfg, 6, table_nat, "OUTPUT", rule_buf);

    LOG_INFO("IPv6 ENHANCE mode setup complete");
    return 0;
}

int tproxy_cleanup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 TPROXY chains");

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j ATP_PRE_0");
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j ATP_OUT_0");

    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value, cfg->network.tcp_port, cfg->network.mark_value);
    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value, cfg->network.mark_value);
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", rule_buf);

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

    tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-j ATP6_PRE_0");
    tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", "-j ATP6_OUT_0");

    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.mark_value6, cfg->network.tcp_port, cfg->network.mark_value6);
    tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", rule_buf);

    snprintf(rule_buf, sizeof(rule_buf), "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             cfg->network.mark_value6, cfg->network.mark_value6);
    tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", rule_buf);

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

static int tproxy_cleanup_enhance_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 ENHANCE mode");

    tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-p tcp -j ATP_REDIRECT_TCP");
    tproxy_rule_del(cfg, 4, "nat", "OUTPUT", "-p tcp -j ATP_REDIRECT_TCP");

    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value, cfg->network.redirect_tcp_port);
    tproxy_rule_del(cfg, 4, "nat", "PREROUTING", rule_buf);
    tproxy_rule_del(cfg, 4, "nat", "OUTPUT", rule_buf);

    tproxy_chain_flush(cfg, 4, "nat", "ATP_REDIRECT_TCP");
    tproxy_chain_destroy(cfg, 4, "nat", "ATP_REDIRECT_TCP");

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-p udp -j ATP_UDP_TPROXY");
    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value);
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", rule_buf);
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_UDP_TPROXY");
    tproxy_chain_destroy(cfg, 4, "mangle", "ATP_UDP_TPROXY");

    LOG_INFO("IPv4 ENHANCE mode cleanup complete");
    return 0;
}

static int tproxy_cleanup_enhance_ipv6(atp_config_t *cfg) {
    if (!cfg->network.proxy_ipv6) return 0;

    LOG_INFO("Cleaning up IPv6 ENHANCE mode");

    tproxy_rule_del(cfg, 6, "nat", "PREROUTING", "-p tcp -j ATP6_REDIRECT_TCP");
    tproxy_rule_del(cfg, 6, "nat", "OUTPUT", "-p tcp -j ATP6_REDIRECT_TCP");

    char rule_buf[256];
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             cfg->network.mark_value6, cfg->network.redirect_tcp_port);
    tproxy_rule_del(cfg, 6, "nat", "PREROUTING", rule_buf);
    tproxy_rule_del(cfg, 6, "nat", "OUTPUT", rule_buf);

    tproxy_chain_flush(cfg, 6, "nat", "ATP6_REDIRECT_TCP");
    tproxy_chain_destroy(cfg, 6, "nat", "ATP6_REDIRECT_TCP");

    tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-p udp -j ATP6_UDP_TPROXY");
    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->network.mark_value6);
    tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", rule_buf);
    tproxy_chain_flush(cfg, 6, "mangle", "ATP6_UDP_TPROXY");
    tproxy_chain_destroy(cfg, 6, "mangle", "ATP6_UDP_TPROXY");

    LOG_INFO("IPv6 ENHANCE mode cleanup complete");
    return 0;
}

int tproxy_sound_bypass(atp_config_t *cfg) { return 0; }

int tproxy_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Setting up XFRM bypass for VPN traffic");

    tproxy_chain_create(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");

    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p esp -j RETURN");
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 500 -j RETURN");

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, "-j XFRM_BYPASS");

    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j XFRM_BYPASS");
    tproxy_rule_insert(cfg, 4, "mangle", "OUTPUT", 1, "-j XFRM_BYPASS");

    tproxy_chain_create(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    tproxy_rule_add(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p esp -j RETURN");
    tproxy_rule_add(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_add(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p udp --dport 500 -j RETURN");

    tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT");
    tproxy_rule_insert(cfg, 4, "nat", "PREROUTING", 1, "-j XFRM_BYPASS_NAT");

    tproxy_rule_del(cfg, 4, "nat", "OUTPUT", "-j XFRM_BYPASS_NAT");
    tproxy_rule_insert(cfg, 4, "nat", "OUTPUT", 1, "-j XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass configured for both mangle and nat tables");
    return 0;
}

int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Cleaning up XFRM bypass chains");

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j XFRM_BYPASS");
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_destroy(cfg, 4, "mangle", "XFRM_BYPASS");

    tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT");
    tproxy_rule_del(cfg, 4, "nat", "OUTPUT", "-j XFRM_BYPASS_NAT");
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_destroy(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass chains cleaned up");
    return 0;
}

int tproxy_cleanup_all(atp_config_t *cfg) {
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
            tproxy_cleanup_ipv4(cfg);
            if (cfg->network.proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
        default:
            tproxy_cleanup_ipv4(cfg);
            if (cfg->network.proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
    }
    return 0;
}

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode) {
    if (cfg->network.dns_hijack == DNS_HIJACK_OFF) return 0;
    if (mode == DNS_HIJACK_OFF) return 0;

    LOG_INFO("Setting up DNS hijack for IPv%d (mode=%d)", family, mode);

    const char *dns_rule = NULL;
    char rule_buf[128];

    if (mode == DNS_HIJACK_TPROXY) {
        snprintf(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
                 cfg->network.dns_port, cfg->network.mark_value);
        dns_rule = rule_buf;
    } else if (mode == DNS_HIJACK_REDIRECT) {
        snprintf(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j REDIRECT --to-ports %d",
                 cfg->network.dns_port);
        dns_rule = rule_buf;
    }

    if (dns_rule) {
        if (family == 4) {
            tproxy_rule_add(cfg, 4, "mangle", "ATP_DNS_PRE_0", dns_rule);
            tproxy_rule_add(cfg, 4, "mangle", "ATP_DNS_OUT_0", dns_rule);
        } else {
            if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
            tproxy_rule_add(cfg, 6, "mangle", "ATP6_DNS_PRE_0", dns_rule);
            tproxy_rule_add(cfg, 6, "mangle", "ATP6_DNS_OUT_0", dns_rule);
        }
    }

    return 0;
}

int tproxy_dns_hijack_cleanup(atp_config_t *cfg, int family) {
    if (family == 4) {
        tproxy_chain_flush(cfg, 4, "mangle", "ATP_DNS_PRE_0");
        tproxy_chain_flush(cfg, 4, "mangle", "ATP_DNS_OUT_0");
        tproxy_chain_destroy(cfg, 4, "mangle", "ATP_DNS_PRE_0");
        tproxy_chain_destroy(cfg, 4, "mangle", "ATP_DNS_OUT_0");
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_DNS_PRE_0");
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_DNS_OUT_0");
        tproxy_chain_destroy(cfg, 6, "mangle", "ATP6_DNS_PRE_0");
        tproxy_chain_destroy(cfg, 6, "mangle", "ATP6_DNS_OUT_0");
    }
    return 0;
}

static int tproxy_reject_or_drop(atp_config_t *cfg, int family, const char *chain, const char *rule) {
    int reject_avail = tproxy_reject_available();

    char modified_rule[256];
    if (reject_avail) {
        snprintf(modified_rule, sizeof(modified_rule), "%s -j REJECT", rule);
    } else {
        snprintf(modified_rule, sizeof(modified_rule), "%s -j DROP", rule);
    }

    return tproxy_rule_add(cfg, family, "filter", chain, modified_rule);
}

int tproxy_block_quic(atp_config_t *cfg, int enable) {
    if (enable) {
        LOG_INFO("Enabling QUIC blocking");

        tproxy_chain_create(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");

        if (cfg->ebpf.ready) {
            const char *pin_dir = boxbpf_pin_dir();
            char bpf_rule[512];
            snprintf(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/box_cidr_out4 -p udp --dport 443 -j REJECT",
                     pin_dir);
            tproxy_rule_add(cfg, 4, "filter", "ATP_QUIC_0", bpf_rule);
            LOG_DEBUG("QUIC blocking: eBPF");
        } else {
            tproxy_reject_or_drop(cfg, 4, "ATP_QUIC_0", "-p udp --dport 443");
            LOG_DEBUG("QUIC blocking: ipset fallback");
        }

        tproxy_rule_add(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_add(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_add(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            tproxy_chain_create(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");

            if (cfg->ebpf.ready) {
                const char *pin_dir = boxbpf_pin_dir();
                char bpf_rule[512];
                snprintf(bpf_rule, sizeof(bpf_rule),
                         "-m bpf --object-pinned %s/box_cidr_out6 -p udp --dport 443 -j REJECT",
                         pin_dir);
                tproxy_rule_add(cfg, 6, "filter", "ATP6_QUIC_0", bpf_rule);
            } else {
                tproxy_reject_or_drop(cfg, 6, "ATP6_QUIC_0", "-p udp --dport 443");
            }

            tproxy_rule_add(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            tproxy_rule_add(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            tproxy_rule_add(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
        }
    } else {
        LOG_INFO("Disabling QUIC blocking");

        tproxy_rule_del(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_del(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_del(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_destroy(cfg, 4, "filter", "ATP_QUIC_0");

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            tproxy_rule_del(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            tproxy_rule_del(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            tproxy_rule_del(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
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
        snprintf(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->network.tcp_port);
        tproxy_rule_add(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            snprintf(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->network.tcp_port);
            tproxy_rule_add(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    } else {
        LOG_INFO("Disabling loopback protection");
        snprintf(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->network.tcp_port);
        tproxy_rule_del(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->network.proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            snprintf(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->network.tcp_port);
            tproxy_rule_del(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    }

    return 0;
}

int tproxy_prevent_loop(atp_config_t *cfg) {
    char rule_buf[64];
    snprintf(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->network.mark_value);

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", rule_buf);
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, rule_buf);

    return 0;
}
