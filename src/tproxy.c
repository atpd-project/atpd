#include "tproxy.h"
#include "logger.h"
#include "utils.h"
#include "config.h"
#include "atp.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
/* P2: Cached TPROXY support check result */
static int g_tproxy_supported = -1;
#define IPTABLES_CMD "/system/bin/iptables"
#define IP6TABLES_CMD "/system/bin/ip6tables"

static int exec_iptables(atp_config_t *cfg, const char *table, const char *cmd, const char *chain, const char *rule) {
    if (cfg->dry_run) {
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
    if (cfg->dry_run) {
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

static int chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain) {
    char cmd[MAX_CMD_LEN];
    char output[256];
    (void)cfg;
    
    if (family == 4) {
        snprintf(cmd, sizeof(cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                 IPTABLES_CMD, table, chain);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        snprintf(cmd, sizeof(cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                 IP6TABLES_CMD, table, chain);
    }
    
    if (exec_cmd(cmd, output, sizeof(output), 5) == 0 && output[0] != '\0') {
        return 1;
    }
    return 0;
}

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (chain_exists(cfg, family, table, chain)) {
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
    if (!chain_exists(cfg, family, table, chain)) {
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
    if (!chain_exists(cfg, family, table, chain)) {
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
    if (family == 4) {
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s",
                 IPTABLES_CMD, table, chain, position, rule);
        return exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        char cmd[MAX_CMD_LEN];
        snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s",
                 IP6TABLES_CMD, table, chain, position, rule);
        return exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
    }
}

int tproxy_atomic_switch(atp_config_t *cfg, int family, const char *table,
                         const char *hook, const char *chain0, const char *chain1) {
    char rule_buf[256];
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain1);
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain0);
    tproxy_rule_del(cfg, family, table, hook, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain1);
    return tproxy_rule_insert(cfg, family, table, hook, 1, rule_buf);
}

int tproxy_support_check(atp_config_t *cfg) {
    /* Return cached result if already checked */
    if (g_tproxy_supported >= 0) return g_tproxy_supported;

    if (cfg->dry_run) {
        g_tproxy_supported = 1;
        return 1;
    }

    LOG_INFO("Running TPROXY support check (cached for lifetime)...");

    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "%s -t mangle -N ATP_TEST 2>/dev/null", IPTABLES_CMD);
    exec_cmd_simple(cmd, 5);

    snprintf(cmd, sizeof(cmd), "%s -t mangle -A ATP_TEST -p tcp -j TPROXY --on-port 1536 --tproxy-mark 20 2>/dev/null",
             IPTABLES_CMD);
    int ret = exec_cmd_simple(cmd, 5);

    exec_cmd_simple(IPTABLES_CMD " -t mangle -F ATP_TEST 2>/dev/null", 5);
    exec_cmd_simple(IPTABLES_CMD " -t mangle -X ATP_TEST 2>/dev/null", 5);

    g_tproxy_supported = (ret == 0) ? 1 : 0;
    LOG_INFO("TPROXY support: %s", g_tproxy_supported ? "YES" : "NO");
    return g_tproxy_supported;
}
static int tproxy_configure_rp_filter(atp_config_t *cfg) {
    DIR *dir;
    struct dirent *entry;
    char path[256];
    int success = 0;
    
    if (cfg->dry_run) {
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
        }
    }
    
    closedir(dir);
    LOG_INFO("rp_filter set to 2 for %d interfaces", success);
    return 0;
}

static int tproxy_reject_available(void) {
    char output[256];
    int ret = exec_cmd("iptables -j REJECT -A ATP_TEST_REJECT 2>&1 | head -1", 
                       output, sizeof(output), 3);
    exec_cmd_simple("iptables -D ATP_TEST_REJECT -j REJECT 2>/dev/null", 3);
    
    if (ret != 0 || strstr(output, "No chain/target/match")) {
        return 0;
    }
    return 1;
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
        snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m socket --transparent -j %s", divert_chain);
    } else {
        snprintf(chain_name, sizeof(chain_name), "ATP_PRE_0");
        snprintf(rule_buf, sizeof(rule_buf), "-p tcp -m socket --transparent -j %s", divert_chain);
    }
    
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

static void tproxy_hook_main_chains(atp_config_t *cfg, int family, const char *suffix) {
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
             cfg->core_user, cfg->core_group);
    tproxy_rule_insert(cfg, family, "mangle", "OUTPUT", 1, hook_rule);

    snprintf(hook_rule, sizeof(hook_rule), "-j %s", pre_chain);
    tproxy_rule_insert(cfg, family, "mangle", "PREROUTING", 1, hook_rule);

    snprintf(hook_rule, sizeof(hook_rule), "-j %s", out_chain);
    tproxy_rule_insert(cfg, family, "mangle", "OUTPUT", 1, hook_rule);
}

/* ========== P1: Batch Rule Injection ========== */

static int tproxy_restore_batch(const char *rules, int family) {
    const char *cmd = (family == 4) ? "iptables-restore -n" : "ip6tables-restore -n";
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

int tproxy_setup_ipv4_batch(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 TPROXY chains (batch mode)");

    tproxy_configure_rp_filter(cfg);

    char rules[8192];
    int offset = 0;

    offset += snprintf(rules + offset, sizeof(rules) - offset,
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

    /* Divert chain rules */
    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A ATP_DIVERT_0 -j MARK --set-mark %d\n"
        "-A ATP_DIVERT_0 -j ACCEPT\n",
        cfg->mark_value
    );

    /* Socket match rule */
    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A ATP_PRE_0 -p tcp -m socket --transparent -j ATP_DIVERT_0\n"
    );

    /* Chain jumps - PRE */
    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A ATP_PRE_0 -j ATP_PROXY_IP_0\n"
        "-A ATP_PRE_0 -j ATP_BYPASS_IP_0\n"
        "-A ATP_PRE_0 -j ATP_PROXY_IFACE_0\n"
        "-A ATP_PRE_0 -j ATP_MAC_0\n"
        "-A ATP_PRE_0 -j ATP_DNS_PRE_0\n"
    );

    /* Chain jumps - OUT */
    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A ATP_OUT_0 -j ATP_PROXY_IP_0\n"
        "-A ATP_OUT_0 -j ATP_BYPASS_IP_0\n"
        "-A ATP_OUT_0 -j ATP_BYPASS_IFACE_0\n"
        "-A ATP_OUT_0 -j ATP_APP_0\n"
        "-A ATP_OUT_0 -j ATP_DNS_OUT_0\n"
    );

    /* P0: UID bypass */
    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A OUTPUT -m owner --uid-owner %s --gid-owner %s -j RETURN\n",
        cfg->core_user, cfg->core_group
    );

    /* Hook main chains */
    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A PREROUTING -j ATP_PRE_0\n"
        "-A OUTPUT -j ATP_OUT_0\n"
        "COMMIT\n"
    );

    if (offset >= (int)sizeof(rules)) {
        LOG_ERROR("Batch rules too large (%d bytes), falling back to sequential mode", offset);
        return tproxy_setup_ipv4(cfg);
    }

    LOG_DEBUG("Batch rules: %d bytes", offset);
    return tproxy_restore_batch(rules, 4);
}

int tproxy_setup_ipv6_batch(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping");
        return 0;
    }

    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 setup skipped");
        cfg->proxy_ipv6 = 0;
        return 0;
    }

    LOG_INFO("Setting up IPv6 TPROXY chains (batch mode)");

    char rules[8192];
    int offset = 0;

    offset += snprintf(rules + offset, sizeof(rules) - offset,
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

    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A ATP6_DIVERT_0 -j MARK --set-mark %d\n"
        "-A ATP6_DIVERT_0 -j ACCEPT\n",
        cfg->mark_value6
    );

    offset += snprintf(rules + offset, sizeof(rules) - offset,
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

    offset += snprintf(rules + offset, sizeof(rules) - offset,
        "-A OUTPUT -m owner --uid-owner %s --gid-owner %s -j RETURN\n"
        "-A PREROUTING -j ATP6_PRE_0\n"
        "-A OUTPUT -j ATP6_OUT_0\n"
        "COMMIT\n",
        cfg->core_user, cfg->core_group
    );

    if (offset >= (int)sizeof(rules)) {
        LOG_ERROR("Batch rules too large (%d bytes), falling back to sequential mode", offset);
        return tproxy_setup_ipv6(cfg);
    }

    LOG_DEBUG("Batch rules: %d bytes", offset);
    return tproxy_restore_batch(rules, 6);
}

int tproxy_setup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 TPROXY chains");
    
    tproxy_configure_rp_filter(cfg);
    
    tproxy_create_standard_chains(cfg, 4, "");
    
    tproxy_setup_divert_chain(cfg, 4, "", cfg->mark_value);
    
    tproxy_setup_socket_match(cfg, 4, "", "ATP_DIVERT_0");
    
    tproxy_setup_chain_jumps(cfg, 4, "", 1);
    
    tproxy_hook_main_chains(cfg, 4, "");
    
    LOG_INFO("IPv4 TPROXY setup complete with DIVERT optimization");
    return 0;
}
int tproxy_setup_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping");
        return 0;
    }
    
    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 setup skipped");
        cfg->proxy_ipv6 = 0;
        return 0;
    }
    
    LOG_INFO("Setting up IPv6 TPROXY chains");
    
    tproxy_create_standard_chains(cfg, 6, "6");
    
    tproxy_setup_divert_chain(cfg, 6, "6", cfg->mark_value6);
    
    tproxy_setup_socket_match(cfg, 6, "6", "ATP6_DIVERT_0");
    
    tproxy_setup_chain_jumps(cfg, 6, "6", 1);
    
    tproxy_hook_main_chains(cfg, 6, "6");
    
    LOG_INFO("IPv6 TPROXY setup complete");
    return 0;
}

/* REDIRECT mode setup */
int tproxy_setup_redirect_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up IPv4 REDIRECT chains");
    
    const char *table = "nat";
    char chain_name[64];
    char rule_buf[256];
    
    snprintf(chain_name, sizeof(chain_name), "ATP_REDIRECT");
    tproxy_chain_create(cfg, 4, table, chain_name);
    tproxy_chain_flush(cfg, 4, table, chain_name);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->tcp_port);
    tproxy_rule_add(cfg, 4, table, chain_name, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain_name);
    tproxy_rule_insert(cfg, 4, table, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 4, table, "OUTPUT", 1, rule_buf);
    
    LOG_INFO("IPv4 REDIRECT setup complete");
    return 0;
}

int tproxy_setup_redirect_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) return 0;
    
    LOG_INFO("Setting up IPv6 REDIRECT chains");
    
    const char *table = "nat";
    char chain_name[64];
    char rule_buf[256];
    
    snprintf(chain_name, sizeof(chain_name), "ATP6_REDIRECT");
    tproxy_chain_create(cfg, 6, table, chain_name);
    tproxy_chain_flush(cfg, 6, table, chain_name);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->tcp_port);
    tproxy_rule_add(cfg, 6, table, chain_name, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-j %s", chain_name);
    tproxy_rule_insert(cfg, 6, table, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 6, table, "OUTPUT", 1, rule_buf);
    
    LOG_INFO("IPv6 REDIRECT setup complete");
    return 0;
}

/* ENHANCE mode setup (TCP:REDIRECT, UDP:TPROXY) */
int tproxy_setup_enhance_ipv4(atp_config_t *cfg) {
    LOG_INFO("Setting up ENHANCE mode for IPv4 (TCP=REDIRECT:%d, UDP=TPROXY:%d)",
             cfg->redirect_tcp_port, cfg->udp_port);
    
    const char *table_mangle = "mangle";
    const char *table_nat = "nat";
    char rule_buf[256];
    char chain_name[64];
    
    /* TCP: REDIRECT in nat table */
    LOG_INFO("Setting up TCP REDIRECT chain (port %d)", cfg->redirect_tcp_port);
    
    snprintf(chain_name, sizeof(chain_name), "ATP_REDIRECT_TCP");
    tproxy_chain_create(cfg, 4, table_nat, chain_name);
    tproxy_chain_flush(cfg, 4, table_nat, chain_name);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", 
             cfg->redirect_tcp_port);
    tproxy_rule_add(cfg, 4, table_nat, chain_name, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j %s", chain_name);
    tproxy_rule_insert(cfg, 4, table_nat, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 4, table_nat, "OUTPUT", 1, rule_buf);
    
    /* UDP: TPROXY in mangle table */
    LOG_INFO("Setting up UDP TPROXY chain (port %d, mark %d)", 
             cfg->udp_port, cfg->mark_value);
    
    snprintf(chain_name, sizeof(chain_name), "ATP_UDP_TPROXY");
    tproxy_chain_create(cfg, 4, table_mangle, chain_name);
    tproxy_chain_flush(cfg, 4, table_mangle, chain_name);
    
    snprintf(rule_buf, sizeof(rule_buf), 
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->udp_port, cfg->mark_value);
    tproxy_rule_add(cfg, 4, table_mangle, chain_name, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j %s", chain_name);
    tproxy_rule_insert(cfg, 4, table_mangle, "PREROUTING", 1, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->mark_value);
    tproxy_rule_insert(cfg, 4, table_mangle, "OUTPUT", 1, rule_buf);
    
    /* Bypass for core user */
    snprintf(rule_buf, sizeof(rule_buf), 
             "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core_user, cfg->core_group);
    tproxy_rule_insert(cfg, 4, table_nat, "OUTPUT", 1, rule_buf);
    
    LOG_INFO("IPv4 ENHANCE mode setup complete");
    return 0;
}

int tproxy_setup_enhance_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) {
        LOG_DEBUG("IPv6 proxy disabled, skipping ENHANCE mode");
        return 0;
    }
    
    LOG_INFO("Setting up ENHANCE mode for IPv6 (TCP=REDIRECT:%d, UDP=TPROXY:%d)",
             cfg->redirect_tcp_port, cfg->udp_port);
    
    const char *table_mangle = "mangle";
    const char *table_nat = "nat";
    char rule_buf[256];
    char chain_name[64];
    
    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_WARN("ip6tables not found, IPv6 ENHANCE mode skipped");
        return 0;
    }
    
    /* TCP: REDIRECT in nat table */
    LOG_INFO("Setting up IPv6 TCP REDIRECT chain (port %d)", cfg->redirect_tcp_port);
    
    snprintf(chain_name, sizeof(chain_name), "ATP6_REDIRECT_TCP");
    tproxy_chain_create(cfg, 6, table_nat, chain_name);
    tproxy_chain_flush(cfg, 6, table_nat, chain_name);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", 
             cfg->redirect_tcp_port);
    tproxy_rule_add(cfg, 6, table_nat, chain_name, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p tcp -j %s", chain_name);
    tproxy_rule_insert(cfg, 6, table_nat, "PREROUTING", 1, rule_buf);
    tproxy_rule_insert(cfg, 6, table_nat, "OUTPUT", 1, rule_buf);
    
    /* UDP: TPROXY in mangle table */
    LOG_INFO("Setting up IPv6 UDP TPROXY chain (port %d, mark %d)", 
             cfg->udp_port, cfg->mark_value6);
    
    snprintf(chain_name, sizeof(chain_name), "ATP6_UDP_TPROXY");
    tproxy_chain_create(cfg, 6, table_mangle, chain_name);
    tproxy_chain_flush(cfg, 6, table_mangle, chain_name);
    
    snprintf(rule_buf, sizeof(rule_buf), 
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->udp_port, cfg->mark_value6);
    tproxy_rule_add(cfg, 6, table_mangle, chain_name, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j %s", chain_name);
    tproxy_rule_insert(cfg, 6, table_mangle, "PREROUTING", 1, rule_buf);
    
    snprintf(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", cfg->mark_value6);
    tproxy_rule_insert(cfg, 6, table_mangle, "OUTPUT", 1, rule_buf);
    
    LOG_INFO("IPv6 ENHANCE mode setup complete");
    return 0;
}

int tproxy_cleanup_ipv4(atp_config_t *cfg) {
    LOG_INFO("Cleaning up IPv4 TPROXY chains");
    
    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j ATP_PRE_0");
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-j ATP_OUT_0");
    
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
    if (!cfg->proxy_ipv6) return 0;
    
    LOG_INFO("Cleaning up IPv6 TPROXY chains");
    
    if (access(IP6TABLES_CMD, X_OK) != 0) {
        LOG_DEBUG("ip6tables not found, skipping IPv6 cleanup");
        return 0;
    }
    
    tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-j ATP6_PRE_0");
    tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", "-j ATP6_OUT_0");
    
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
    tproxy_chain_flush(cfg, 4, "nat", "ATP_REDIRECT_TCP");
    tproxy_chain_destroy(cfg, 4, "nat", "ATP_REDIRECT_TCP");

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-p udp -j ATP_UDP_TPROXY");
    tproxy_rule_del(cfg, 4, "mangle", "OUTPUT", "-p udp -j MARK --set-mark 20");
    tproxy_chain_flush(cfg, 4, "mangle", "ATP_UDP_TPROXY");
    tproxy_chain_destroy(cfg, 4, "mangle", "ATP_UDP_TPROXY");

    LOG_INFO("IPv4 ENHANCE mode cleanup complete");
    return 0;
}

static int tproxy_cleanup_enhance_ipv6(atp_config_t *cfg) {
    if (!cfg->proxy_ipv6) return 0;

    LOG_INFO("Cleaning up IPv6 ENHANCE mode");

    tproxy_rule_del(cfg, 6, "nat", "PREROUTING", "-p tcp -j ATP6_REDIRECT_TCP");
    tproxy_rule_del(cfg, 6, "nat", "OUTPUT", "-p tcp -j ATP6_REDIRECT_TCP");
    tproxy_chain_flush(cfg, 6, "nat", "ATP6_REDIRECT_TCP");
    tproxy_chain_destroy(cfg, 6, "nat", "ATP6_REDIRECT_TCP");

    tproxy_rule_del(cfg, 6, "mangle", "PREROUTING", "-p udp -j ATP6_UDP_TPROXY");
    tproxy_rule_del(cfg, 6, "mangle", "OUTPUT", "-p udp -j MARK --set-mark 25");
    tproxy_chain_flush(cfg, 6, "mangle", "ATP6_UDP_TPROXY");
    tproxy_chain_destroy(cfg, 6, "mangle", "ATP6_UDP_TPROXY");

    LOG_INFO("IPv6 ENHANCE mode cleanup complete");
    return 0;
}

/* Setup XFRM bypass for VPN traffic in both mangle and nat tables */
int tproxy_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Setting up XFRM bypass for VPN traffic");

    /* ============================================ */
    /* mangle table bypass (UDP traffic) */
    /* ============================================ */
    tproxy_chain_create(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");

    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p esp -j RETURN");
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_add(cfg, 4, "mangle", "XFRM_BYPASS", "-p udp --dport 500 -j RETURN");

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, "-j XFRM_BYPASS");

    /* ============================================ */
    /* nat table bypass (TCP traffic) - for MODE 3 compatibility */
    /* ============================================ */
    tproxy_chain_create(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    tproxy_rule_add(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p esp -j RETURN");
    tproxy_rule_add(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p udp --dport 4500 -j RETURN");
    tproxy_rule_add(cfg, 4, "nat", "XFRM_BYPASS_NAT", "-p udp --dport 500 -j RETURN");

    tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT");
    tproxy_rule_insert(cfg, 4, "nat", "PREROUTING", 1, "-j XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass configured for both mangle and nat tables");
    return 0;
}

/* Cleanup XFRM bypass chains from both mangle and nat tables */
int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Cleaning up XFRM bypass chains");

    /* Cleanup mangle table bypass */
    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_destroy(cfg, 4, "mangle", "XFRM_BYPASS");

    /* Cleanup nat table bypass */
    tproxy_rule_del(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT");
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_destroy(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass chains cleaned up");
    return 0;
}

int tproxy_cleanup_all(atp_config_t *cfg) {
    /* Cleanup XFRM bypass first (independent of mode) */
    tproxy_cleanup_xfrm_bypass(cfg);

    /* Cleanup based on proxy mode */
    switch (cfg->proxy_mode) {
        case MODE_ENHANCE:
            tproxy_cleanup_enhance_ipv4(cfg);
            if (cfg->proxy_ipv6) tproxy_cleanup_enhance_ipv6(cfg);
            break;
        case MODE_TPROXY:
            tproxy_cleanup_ipv4(cfg);
            if (cfg->proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
        case MODE_REDIRECT:
            tproxy_cleanup_ipv4(cfg);
            if (cfg->proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
        default:
            tproxy_cleanup_ipv4(cfg);
            if (cfg->proxy_ipv6) tproxy_cleanup_ipv6(cfg);
            break;
    }
    return 0;
}

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode) {
    if (cfg->dns_hijack == DNS_HIJACK_OFF) return 0;
    if (mode == DNS_HIJACK_OFF) return 0;

    LOG_INFO("Setting up DNS hijack for IPv%d (mode=%d)", family, mode);

    const char *dns_rule = NULL;
    char rule_buf[128];

    if (mode == DNS_HIJACK_TPROXY) {
        snprintf(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
                 cfg->dns_port, cfg->mark_value);
        dns_rule = rule_buf;
    } else if (mode == DNS_HIJACK_REDIRECT) {
        snprintf(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j REDIRECT --to-ports %d",
                 cfg->dns_port);
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
    } else {
        if (access(IP6TABLES_CMD, X_OK) != 0) return 0;
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_DNS_PRE_0");
        tproxy_chain_flush(cfg, 6, "mangle", "ATP6_DNS_OUT_0");
    }
    return 0;
}

static int tproxy_reject_or_drop(atp_config_t *cfg, int family, const char *chain, const char *rule) {
    static int reject_available = -1;

    if (reject_available == -1) {
        reject_available = tproxy_reject_available();
        if (reject_available) {
            LOG_INFO("REJECT target is available");
        } else {
            LOG_WARN("REJECT target not available, using DROP fallback");
        }
    }

    char modified_rule[256];
    if (reject_available) {
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

        tproxy_reject_or_drop(cfg, 4, "ATP_QUIC_0", "-p udp --dport 443");
        tproxy_rule_add(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_add(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_add(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");

        if (cfg->proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            tproxy_chain_create(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_reject_or_drop(cfg, 6, "ATP6_QUIC_0", "-p udp --dport 443");
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

        if (cfg->proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
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
                 cfg->tcp_port);
        tproxy_rule_add(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            snprintf(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->tcp_port);
            tproxy_rule_add(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    } else {
        LOG_INFO("Disabling loopback protection");
        snprintf(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT",
                 cfg->tcp_port);
        tproxy_rule_del(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->proxy_ipv6 && access(IP6TABLES_CMD, X_OK) == 0) {
            snprintf(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT",
                     cfg->tcp_port);
            tproxy_rule_del(cfg, 6, "filter", "OUTPUT", rule_buf);
        }
    }

    return 0;
}

int tproxy_prevent_loop(atp_config_t *cfg) {
    char rule_buf[64];
    snprintf(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->mark_value);

    tproxy_rule_del(cfg, 4, "mangle", "PREROUTING", rule_buf);
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, rule_buf);

    return 0;
}

int tproxy_refresh_rules(atp_config_t *cfg) {
    if (!cfg) return -1;

    LOG_INFO("[TPROXY] Refreshing all TPROXY rules");

    tproxy_cleanup_all(cfg);
    tproxy_configure_rp_filter(cfg);
    tproxy_setup_ipv4_batch(cfg);
    tproxy_setup_ipv6_batch(cfg);

    return 0;
}

int ip_rule_audit(atp_config_t *cfg) {
    if (!cfg) return -1;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip rule show | grep -q 'fwmark %d'", cfg->tproxy_mark);

    if (exec_cmd_simple(cmd, 2) != 0) {
        LOG_WARN("[TPROXY] IP rule for fwmark %d missing, restoring", cfg->tproxy_mark);
        snprintf(cmd, sizeof(cmd), "ip rule add fwmark %d table %d pref %d",
                 cfg->tproxy_mark, cfg->tproxy_table, cfg->tproxy_pref);
        exec_cmd_simple(cmd, 2);
    }

    return 0;
}
