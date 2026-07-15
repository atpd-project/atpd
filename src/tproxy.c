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

static int g_ipv4_available = -1;
static int g_ipv6_available = -1;
static pthread_mutex_t g_avail_mutex = PTHREAD_MUTEX_INITIALIZER;

#define IPTABLES_CMD "/system/bin/iptables"
#define IP6TABLES_CMD "/system/bin/ip6tables"
#define IPTABLES_SAVE_CMD "/system/bin/iptables-save"
#define IP6TABLES_SAVE_CMD "/system/bin/ip6tables-save"

#define MAX_ORPHAN_CHAINS 128

static const char *chain_suffixes[] = {
    "PRE_0", "PRE_1",
    "OUT_0", "OUT_1",
    "DIVERT_0", "DIVERT_1",
    "PROXY_IP_0", "PROXY_IP_1",
    "BYPASS_IP_0", "BYPASS_IP_1",
    "PROXY_IFACE_0", "PROXY_IFACE_1",
    "BYPASS_IFACE_0", "BYPASS_IFACE_1",
    "DNS_PRE_0", "DNS_PRE_1",
    "DNS_OUT_0", "DNS_OUT_1",
    "APP_0", "APP_1",
    "MAC_0", "MAC_1",
    NULL

static const char *extra_chains_v4[] = {
    "ATP_QUIC_0",
    "ATP_REDIRECT",
    "ATP_REDIRECT_TCP",
    "ATP_UDP_TPROXY",
    "XFRM_BYPASS",
    "XFRM_BYPASS_NAT",
    NULL
};   

static const char *extra_chains_v6[] = {
    "ATP6_QUIC_0",
    "ATP6_REDIRECT",
    "ATP6_REDIRECT_TCP",
    "ATP6_UDP_TPROXY",
    NULL
};   

typedef struct {
    int family;
    const char *cmd;
    const char *save_cmd;
    const char *prefix;
    int (*enabled)(atp_config_t *cfg);
    int (*mark)(atp_config_t *cfg);
    const char *(*hotspot_subnet)(atp_config_t *cfg);
    const char *(*bypass_list)(atp_config_t *cfg);
    const char *(*proxy_list)(atp_config_t *cfg);
    const char *(*pin_out)(atp_config_t *cfg);
    const char *(*pin_pre)(atp_config_t *cfg);
    const char *(*ipset)(atp_config_t *cfg);

static int family_enabled_4(atp_config_t *cfg) { (void)cfg; return 1; }
static int family_enabled_6(atp_config_t *cfg) { return cfg->network.proxy_ipv6; }
static int family_mark_4(atp_config_t *cfg) { return cfg->network.mark_value; }
static int family_mark_6(atp_config_t *cfg) { return cfg->network.mark_value6; }
static const char *family_hotspot_4(atp_config_t *cfg) { return cfg->interface.hotspot_subnet_ipv4; }
static const char *family_hotspot_6(atp_config_t *cfg) { return cfg->interface.hotspot_subnet_ipv6; }
static const char *family_bypass_4(atp_config_t *cfg) { return cfg->iplist.bypass_ipv4_list; }
static const char *family_bypass_6(atp_config_t *cfg) { return cfg->iplist.bypass_ipv6_list; }
static const char *family_proxy_4(atp_config_t *cfg) { return cfg->iplist.proxy_ipv4_list; }
static const char *family_proxy_6(atp_config_t *cfg) { return cfg->iplist.proxy_ipv6_list; }
static const char *family_pin_out_4(atp_config_t *cfg) { (void)cfg; return "box_cidr_out4"; }
static const char *family_pin_out_6(atp_config_t *cfg) { (void)cfg; return "box_cidr_out6"; }
static const char *family_pin_pre_4(atp_config_t *cfg) { (void)cfg; return "box_cidr_pre4"; }
static const char *family_pin_pre_6(atp_config_t *cfg) { (void)cfg; return "box_cidr_pre6"; }
static const char *family_ipset_4(atp_config_t *cfg) { (void)cfg; return "cnip"; }
static const char *family_ipset_6(atp_config_t *cfg) { (void)cfg; return "cnip6"; }

static const tproxy_family_ctx_t family4 = {
    .family = 4,
    .cmd = IPTABLES_CMD,
    .save_cmd = IPTABLES_SAVE_CMD,
    .prefix = "ATP",
    .enabled = family_enabled_4,
    .mark = family_mark_4,
    .hotspot_subnet = family_hotspot_4,
    .bypass_list = family_bypass_4,
    .proxy_list = family_proxy_4,
    .pin_out = family_pin_out_4,
    .pin_pre = family_pin_pre_4,
    .ipset = family_ipset_4,

static const tproxy_family_ctx_t family6 = {
    .family = 6,
    .cmd = IP6TABLES_CMD,
    .save_cmd = IP6TABLES_SAVE_CMD,
    .prefix = "ATP6",
    .enabled = family_enabled_6,
    .mark = family_mark_6,
    .hotspot_subnet = family_hotspot_6,
    .bypass_list = family_bypass_6,
    .proxy_list = family_proxy_6,
    .pin_out = family_pin_out_6,
    .pin_pre = family_pin_pre_6,
    .ipset = family_ipset_6,

static const tproxy_family_ctx_t *get_ctx(int family) {
    switch (family) {
        case 4: return &family4;
        case 6: return &family6;
        default: return NULL;

static int validate_iface_name(const char *name) {
    if (!name || !*name) return -1;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-' && *p != ':') {
            return -1;
    return 0;

static int validate_ip_or_cidr(const char *str) {
    if (!str || !*str) return -1;

    struct in_addr v4;
    struct in6_addr v6;

    if (inet_pton(AF_INET, str, &v4) == 1) return 0;
    if (inet_pton(AF_INET6, str, &v6) == 1) return 0;

    char ip[128];
    int prefix = -1;
    if (sscanf(str, "%127[^/]/%d", ip, &prefix) == 2) {
        if (inet_pton(AF_INET, ip, &v4) == 1 && prefix >= 0 && prefix <= 32) return 0;
        if (inet_pton(AF_INET6, ip, &v6) == 1 && prefix >= 0 && prefix <= 128) return 0;

    return -1;

static int family_available(int family) {
    int *cache;
    const char *path;

    pthread_mutex_lock(&g_avail_mutex);

    if (family == 4) {
        cache = &g_ipv4_available;
        path = IPTABLES_CMD;
        cache = &g_ipv6_available;
        path = IP6TABLES_CMD;
        pthread_mutex_unlock(&g_avail_mutex);
        return 0;

    if (*cache < 0) {
        *cache = (access(path, X_OK) == 0) ? 1 : 0;

    pthread_mutex_unlock(&g_avail_mutex);
    return *cache;

static void build_chain_name(int family, const char *suffix, char *buf, size_t len) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);

    if (!ctx) {
        if (len > 0) buf[0] = '\0';
        LOG_ERROR("Invalid family: %d", family);
        return;

    SAFE_SNPRINTF(buf, len, "%s_%s", ctx->prefix, suffix);

static int is_known_atp_chain(int family, const char *chain) {
    char expected[64];

    for (int i = 0; chain_suffixes[i] != NULL; i++) {
        build_chain_name(family, chain_suffixes[i], expected, sizeof(expected));
        if (expected[0] == '\0') continue;
        if (strcmp(expected, chain) == 0) return 1;

    const char **extras;
    if (family == 4) {
        extras = extra_chains_v4;
        extras = extra_chains_v6;
        return 0;

    for (int i = 0; extras[i] != NULL; i++) {
        if (strcmp(extras[i], chain) == 0) return 1;

    return 0;

static int exec_ipt(atp_config_t *cfg, int family, const char *table,
                    const char *cmd, const char *chain, const char *rule) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return -1;

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] %s -t %s %s %s %s",
                  ctx->cmd, table, cmd, chain, rule ? rule : "");
        return 0;

    if (!family_available(family)) return -1;

    char command[MAX_CMD_LEN];
    int n;
    if (rule) {
        n = snprintf(command, sizeof(command), "%s -t %s %s %s %s 2>/dev/null",
                     ctx->cmd, table, cmd, chain, rule);
        n = snprintf(command, sizeof(command), "%s -t %s %s %s 2>/dev/null",
                     ctx->cmd, table, cmd, chain);
    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command truncated");
        return -1;

    int ret = exec_cmd_simple(command, CMD_TIMEOUT_SEC);
    if (ret != 0) {
        LOG_DEBUG("%s failed: -t %s %s %s %s (ret=%d)",
                  ctx->cmd, table, cmd, chain, rule ? rule : "", ret);
    return ret;

static int exec_ipt_del_all(atp_config_t *cfg, int family, const char *table,
                            const char *chain, const char *rule) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return -1;

    if (cfg->core.dry_run) return 0;
    if (!family_available(family)) return -1;

    char command[MAX_CMD_LEN];
    int max_attempts = 100;

    while (max_attempts-- > 0) {
        int n = snprintf(command, sizeof(command),
                         "%s -t %s -D %s %s 2>/dev/null",
                         ctx->cmd, table, chain, rule);
        if (n < 0 || n >= (int)sizeof(command)) {
            LOG_ERROR("Command truncated");
            return -1;

        int ret = exec_cmd_simple(command, CMD_TIMEOUT_SEC);
        if (ret != 0) break;

    return 0;

static void delete_all_rules(atp_config_t *cfg, int family, const char *table,
                             const char *chain, const char *rule) {
    exec_ipt_del_all(cfg, family, table, chain, rule);

static int exec_ipt_flush_fast(atp_config_t *cfg, int family, const char *table, const char *chain) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return -1;

    if (cfg->core.dry_run) return 0;
    if (!family_available(family)) return -1;

    char command[MAX_CMD_LEN];
    int n = snprintf(command, sizeof(command), "%s -t %s -F %s 2>/dev/null",
                     ctx->cmd, table, chain);
    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command truncated");
        return -1;

    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);

static int exec_ipt_destroy_fast(atp_config_t *cfg, int family, const char *table, const char *chain) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return -1;

    if (cfg->core.dry_run) return 0;
    if (!family_available(family)) return -1;

    char command[MAX_CMD_LEN];
    int n = snprintf(command, sizeof(command), "%s -t %s -X %s 2>/dev/null",
                     ctx->cmd, table, chain);
    if (n < 0 || n >= (int)sizeof(command)) {
        LOG_ERROR("Command truncated");
        return -1;

    return exec_cmd_simple(command, CMD_TIMEOUT_SEC);

int tproxy_chain_exists(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (cfg->core.dry_run) return 1;

    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return 0;

    if (!family_available(family)) return 0;

    char cmd[MAX_CMD_LEN];
    char output[1024] = {0};
    int n = snprintf(cmd, sizeof(cmd), "%s -t %s -L %s 2>/dev/null | head -1",
                     ctx->cmd, table, chain);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        return 0;

    int ret = exec_cmd(cmd, output, sizeof(output), CMD_TIMEOUT_SEC);
    if (ret != 0) return 0;
    return (strstr(output, "Chain") != NULL) ? 1 : 0;

int tproxy_chain_create(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (tproxy_chain_exists(cfg, family, table, chain)) return 0;
    return exec_ipt(cfg, family, table, "-N", chain, NULL);

int tproxy_chain_flush(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (!tproxy_chain_exists(cfg, family, table, chain)) return 0;
    return exec_ipt_flush_fast(cfg, family, table, chain);

int tproxy_chain_destroy(atp_config_t *cfg, int family, const char *table, const char *chain) {
    if (!tproxy_chain_exists(cfg, family, table, chain)) return 0;
    return exec_ipt_destroy_fast(cfg, family, table, chain);

int tproxy_rule_exists(atp_config_t *cfg, int family, const char *table,
                       const char *chain, const char *rule) {
    if (cfg->core.dry_run) return 0;

    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return 0;

    if (!family_available(family)) return 0;

    char cmd[MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "%s -t %s -C %s %s 2>/dev/null",
                     ctx->cmd, table, chain, rule);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        return 0;
    return (exec_cmd_simple(cmd, CMD_TIMEOUT_SEC) == 0) ? 1 : 0;

int tproxy_rule_ensure_single(atp_config_t *cfg, int family, const char *table,
                              const char *chain, const char *rule) {
    delete_all_rules(cfg, family, table, chain, rule);
    return exec_ipt(cfg, family, table, "-A", chain, rule);

int tproxy_rule_ensure_single_insert(atp_config_t *cfg, int family, const char *table,
                                     const char *chain, int position, const char *rule) {
    delete_all_rules(cfg, family, table, chain, rule);
    return tproxy_rule_insert(cfg, family, table, chain, position, rule);

int tproxy_rule_add(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    if (tproxy_rule_exists(cfg, family, table, chain, rule)) return 0;
    return exec_ipt(cfg, family, table, "-A", chain, rule);

int tproxy_rule_del(atp_config_t *cfg, int family, const char *table,
                    const char *chain, const char *rule) {
    return exec_ipt_del_all(cfg, family, table, chain, rule);

int tproxy_rule_insert(atp_config_t *cfg, int family, const char *table,
                       const char *chain, int position, const char *rule) {
    if (cfg->core.dry_run) return 0;

    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!family_available(family)) return 0;

    char cmd[MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "%s -t %s -I %s %d %s 2>/dev/null",
                     ctx->cmd, table, chain, position, rule);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        return -1;
    int ret = exec_cmd_simple(cmd, CMD_TIMEOUT_SEC);
    if (ret != 0) {
        LOG_ERROR("%s insert failed: -t %s -I %s %d %s (ret=%d)",
                  ctx->cmd, table, chain, position, rule, ret);
    return ret;

int tproxy_support_check(atp_config_t *cfg) {
    pthread_mutex_lock(&g_tproxy_support_mutex);
    if (g_tproxy_supported >= 0) {
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return g_tproxy_supported;

    if (cfg->core.dry_run) {
        g_tproxy_supported = 1;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 1;

    LOG_INFO("Running TPROXY support check...");

    char cmd[MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "%s -t mangle -N ATP_TEST 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    exec_cmd_simple(cmd, 5);

    n = snprintf(cmd, sizeof(cmd), "%s -t mangle -A ATP_TEST -p tcp -j TPROXY --on-port 1536 --tproxy-mark 20 2>/dev/null",
             IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    int ret = exec_cmd_simple(cmd, 5);

    n = snprintf(cmd, sizeof(cmd), "%s -t mangle -F ATP_TEST 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    exec_cmd_simple(cmd, 5);
    n = snprintf(cmd, sizeof(cmd), "%s -t mangle -X ATP_TEST 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_tproxy_supported = 0;
        pthread_mutex_unlock(&g_tproxy_support_mutex);
        return 0;
    exec_cmd_simple(cmd, 5);

    g_tproxy_supported = (ret == 0) ? 1 : 0;
    LOG_INFO("TPROXY support: %s", g_tproxy_supported ? "YES" : "NO");
    pthread_mutex_unlock(&g_tproxy_support_mutex);
    return g_tproxy_supported;

static int tproxy_configure_rp_filter(atp_config_t *cfg) {
    DIR *dir;
    struct dirent *entry;
    char path[256];
    int success = 0;

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] Would set rp_filter=2 for all interfaces");
        return 0;

    LOG_INFO("Configuring rp_filter=2 for TPROXY compatibility");

    exec_cmd_simple("echo 2 > /proc/sys/net/ipv4/conf/all/rp_filter 2>/dev/null", 2);
    exec_cmd_simple("echo 2 > /proc/sys/net/ipv4/conf/default/rp_filter 2>/dev/null", 2);

    dir = opendir("/proc/sys/net/ipv4/conf");
    if (!dir) {
        LOG_WARN("Failed to open /proc/sys/net/ipv4/conf");
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        int n = snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", entry->d_name);
        if (n < 0 || n >= (int)sizeof(path)) {
            LOG_ERROR("Path truncated");
            continue;
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "2\n");
            fclose(fp);
            success++;

    closedir(dir);
    LOG_INFO("rp_filter set to 2 for %d interfaces", success);
    return 0;

static int tproxy_reject_available(void) {
    pthread_mutex_lock(&g_reject_mutex);
    if (g_reject_available >= 0) {
        pthread_mutex_unlock(&g_reject_mutex);
        return g_reject_available;

    char cmd[MAX_CMD_LEN];
    char output[256] = {0};

    int n = snprintf(cmd, sizeof(cmd), "%s -N ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    int ret = exec_cmd_simple(cmd, 3);
    if (ret != 0) {
        n = snprintf(cmd, sizeof(cmd), "%s -X ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            LOG_ERROR("Command truncated");
            g_reject_available = 0;
            pthread_mutex_unlock(&g_reject_mutex);
            return 0;
        exec_cmd_simple(cmd, 3);
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;

    n = snprintf(cmd, sizeof(cmd), "%s -A ATP_TEST_REJECT -j REJECT 2>&1", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    ret = exec_cmd(cmd, output, sizeof(output), 3);

    n = snprintf(cmd, sizeof(cmd), "%s -F ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    exec_cmd_simple(cmd, 3);
    n = snprintf(cmd, sizeof(cmd), "%s -X ATP_TEST_REJECT 2>/dev/null", IPTABLES_CMD);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        g_reject_available = 0;
        pthread_mutex_unlock(&g_reject_mutex);
        return 0;
    exec_cmd_simple(cmd, 3);

    if (ret != 0 || strstr(output, "No chain/target/match") != NULL) {
        g_reject_available = 0;
        g_reject_available = 1;

    pthread_mutex_unlock(&g_reject_mutex);
    return g_reject_available;

static void tproxy_create_standard_chains(atp_config_t *cfg, int family) {
    for (int i = 0; chain_suffixes[i] != NULL; i++) {
        char chain_name[64];
        build_chain_name(family, chain_suffixes[i], chain_name, sizeof(chain_name));
        if (chain_name[0] == '\0') continue;
        tproxy_chain_create(cfg, family, "mangle", chain_name);
        tproxy_chain_flush(cfg, family, "mangle", chain_name);

static void tproxy_setup_divert_chain(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return;

    char chain[64];
    build_chain_name(family, "DIVERT_0", chain, sizeof(chain));
    if (chain[0] == '\0') return;

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j MARK --set-mark %d", ctx->mark(cfg));
    tproxy_rule_ensure_single(cfg, family, "mangle", chain, rule_buf);
    tproxy_rule_ensure_single(cfg, family, "mangle", chain, "-j ACCEPT");

static void tproxy_setup_socket_match(atp_config_t *cfg, int family) {
    char pre_chain[64], divert_chain[64];
    build_chain_name(family, "PRE_0", pre_chain, sizeof(pre_chain));
    build_chain_name(family, "DIVERT_0", divert_chain, sizeof(divert_chain));
    if (pre_chain[0] == '\0' || divert_chain[0] == '\0') return;

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -m socket --transparent -j %s", divert_chain);
    tproxy_rule_ensure_single(cfg, family, "mangle", pre_chain, rule_buf);

static void tproxy_setup_chain_jumps(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return;

    char chains[10][64];
    const char *names[] = {"PRE_0", "OUT_0", "PROXY_IP_0", "BYPASS_IP_0",
                           "PROXY_IFACE_0", "BYPASS_IFACE_0", "MAC_0",
                           "DNS_PRE_0", "DNS_OUT_0", "APP_0"};

    for (int i = 0; i < 10; i++) {
        build_chain_name(family, names[i], chains[i], sizeof(chains[i]));
        if (chains[i][0] == '\0') return;

    struct { const char *chain; const char *target; } jumps[] = {
        {chains[0], chains[2]}, {chains[0], chains[3]},
        {chains[0], chains[4]}, {chains[0], chains[6]},
        {chains[0], chains[7]},
        {chains[1], chains[2]}, {chains[1], chains[3]},
        {chains[1], chains[5]}, {chains[1], chains[9]},
        {chains[1], chains[8]}, {NULL, NULL}

    char rule_buf[256];
    for (int i = 0; jumps[i].chain != NULL; i++) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", jumps[i].target);
        tproxy_rule_ensure_single(cfg, family, "mangle", jumps[i].chain, rule_buf);

static void tproxy_setup_iface_rules(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return;

    char proxy_chain[64], bypass_chain[64];
    build_chain_name(family, "PROXY_IFACE_0", proxy_chain, sizeof(proxy_chain));
    build_chain_name(family, "BYPASS_IFACE_0", bypass_chain, sizeof(bypass_chain));
    if (proxy_chain[0] == '\0' || bypass_chain[0] == '\0') return;

    char rule[256], list_buf[512], *saveptr;

    tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, "-i lo -j RETURN");

    if (validate_iface_name(cfg->interface.mobile_iface) == 0) {
        if (cfg->interface.proxy_mobile) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.mobile_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.mobile_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.mobile_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);

    int hotspot_on_wifi = (strcmp(cfg->interface.hotspot_iface, cfg->interface.wifi_iface) == 0);
    const char *hotspot_subnet = ctx->hotspot_subnet(cfg);

    if (hotspot_on_wifi) {
        if (cfg->interface.proxy_hotspot) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -s %s -j RETURN", cfg->interface.hotspot_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -s %s -j ACCEPT", cfg->interface.hotspot_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);

        if (cfg->interface.proxy_wifi) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s ! -s %s -j RETURN", cfg->interface.wifi_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s ! -s %s -j ACCEPT", cfg->interface.wifi_iface, hotspot_subnet);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
        if (cfg->interface.proxy_wifi) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.wifi_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);

        if (cfg->interface.proxy_hotspot) {
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.hotspot_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.hotspot_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.hotspot_iface);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);

    if (cfg->interface.proxy_usb && validate_iface_name(cfg->interface.usb_iface) == 0) {
        SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", cfg->interface.usb_iface);
        tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", cfg->interface.usb_iface);
        tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
        SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", cfg->interface.usb_iface);
        tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);

    if (cfg->interface.other_proxy[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", cfg->interface.other_proxy);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_iface_name(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j RETURN", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            token = strtok_r(NULL, " ", &saveptr);

    if (cfg->interface.other_bypass[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", cfg->interface.other_bypass);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_iface_name(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-i %s -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
                SAFE_SNPRINTF(rule, sizeof(rule), "-o %s -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            token = strtok_r(NULL, " ", &saveptr);

static void tproxy_setup_ip_rules(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return;

    char proxy_chain[64], bypass_chain[64];
    build_chain_name(family, "PROXY_IP_0", proxy_chain, sizeof(proxy_chain));
    build_chain_name(family, "BYPASS_IP_0", bypass_chain, sizeof(bypass_chain));
    if (proxy_chain[0] == '\0' || bypass_chain[0] == '\0') return;

    char rule[256], list_buf[4096], *saveptr;
    const char *proxy_list = ctx->proxy_list(cfg);
    const char *bypass_list = ctx->bypass_list(cfg);

    if (proxy_list && proxy_list[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", proxy_list);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_ip_or_cidr(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-d %s -j RETURN", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", proxy_chain, rule);
            token = strtok_r(NULL, " ", &saveptr);

    if (bypass_list && bypass_list[0]) {
        int len = snprintf(list_buf, sizeof(list_buf), "%s", bypass_list);
        if (len < 0 || len >= (int)sizeof(list_buf)) {
            LOG_ERROR("list_buf truncated");
            return;
        char *token = strtok_r(list_buf, " ", &saveptr);
        while (token) {
            if (validate_ip_or_cidr(token) == 0) {
                SAFE_SNPRINTF(rule, sizeof(rule), "-d %s -p udp ! --dport 53 -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
                SAFE_SNPRINTF(rule, sizeof(rule), "-d %s ! -p udp -j ACCEPT", token);
                tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            token = strtok_r(NULL, " ", &saveptr);

    if (cfg->filter.bypass_cn_ip) {
        if (cfg->ebpf.ready) {
            const char *pin_dir = boxbpf_pin_dir();
            const char *pin_out = ctx->pin_out(cfg);
            const char *pin_pre = ctx->pin_pre(cfg);
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
            const char *ipset = ctx->ipset(cfg);
            SAFE_SNPRINTF(rule, sizeof(rule), "-m set --match-set %s dst -p udp ! --dport 53 -j ACCEPT", ipset);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            SAFE_SNPRINTF(rule, sizeof(rule), "-m set --match-set %s dst ! -p udp -j ACCEPT", ipset);
            tproxy_rule_ensure_single(cfg, family, "mangle", bypass_chain, rule);
            LOG_DEBUG("CNIP bypass: ipset (%s)", ipset);

void tproxy_hook_main_chains(atp_config_t *cfg, int family) {
    char pre_chain[64], out_chain[64];
    build_chain_name(family, "PRE_0", pre_chain, sizeof(pre_chain));
    build_chain_name(family, "OUT_0", out_chain, sizeof(out_chain));
    if (pre_chain[0] == '\0' || out_chain[0] == '\0') return;

    char hook_rule[128];
    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule),
             "-m owner --uid-owner %s --gid-owner %s -j RETURN",
             cfg->core.core_user, cfg->core.core_group);
    tproxy_rule_ensure_single(cfg, family, "mangle", "OUTPUT", hook_rule);

    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule), "-j %s", pre_chain);
    tproxy_rule_ensure_single_insert(cfg, family, "mangle", "PREROUTING", 1, hook_rule);

    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule), "-j %s", out_chain);
    tproxy_rule_ensure_single_insert(cfg, family, "mangle", "OUTPUT", 1, hook_rule);

static int tproxy_setup_family(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!ctx->enabled(cfg)) {
        LOG_DEBUG("Family %d proxy disabled, skipping", family);
        return 0;

    if (!family_available(family)) {
        if (family == 6) {
            LOG_WARN("ip6tables not found, IPv6 setup skipped");
            cfg->network.proxy_ipv6 = 0;
            return 0;
        return -1;

    LOG_INFO("Setting up TPROXY chains for family %d", family);

    tproxy_create_standard_chains(cfg, family);
    tproxy_setup_divert_chain(cfg, family);
    tproxy_setup_socket_match(cfg, family);
    tproxy_setup_chain_jumps(cfg, family);
    tproxy_setup_iface_rules(cfg, family);
    tproxy_setup_ip_rules(cfg, family);

    int mark = ctx->mark(cfg);
    char rule_buf[256];

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             mark, cfg->network.tcp_port, mark);
    tproxy_rule_ensure_single(cfg, family, "mangle", "PREROUTING", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             mark, mark);
    tproxy_rule_ensure_single(cfg, family, "mangle", "OUTPUT", rule_buf);

    tproxy_hook_main_chains(cfg, family);

    LOG_INFO("TPROXY setup complete for family %d", family);
    return 0;

int tproxy_setup_ipv4(atp_config_t *cfg) {
    tproxy_configure_rp_filter(cfg);
    return tproxy_setup_family(cfg, 4);

int tproxy_setup_ipv6(atp_config_t *cfg) {
    return tproxy_setup_family(cfg, 6);

static int tproxy_setup_redirect_family(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!ctx->enabled(cfg)) {
        LOG_DEBUG("Family %d proxy disabled, skipping REDIRECT", family);
        return 0;

    if (!family_available(family)) {
        if (family == 6) {
            LOG_WARN("ip6tables not found, IPv6 REDIRECT skipped");
            cfg->network.proxy_ipv6 = 0;
            return 0;
        return -1;

    LOG_INFO("Setting up REDIRECT chains for family %d", family);

    char chain[64];
    build_chain_name(family, "REDIRECT", chain, sizeof(chain));
    if (chain[0] == '\0') return -1;

    char rule_buf[256];

    tproxy_chain_create(cfg, family, "nat", chain);
    tproxy_chain_flush(cfg, family, "nat", chain);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d", cfg->network.tcp_port);
    tproxy_rule_ensure_single(cfg, family, "nat", chain, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", chain);
    tproxy_rule_ensure_single_insert(cfg, family, "nat", "PREROUTING", 1, rule_buf);
    tproxy_rule_ensure_single_insert(cfg, family, "nat", "OUTPUT", 1, rule_buf);

    LOG_INFO("REDIRECT setup complete for family %d", family);
    return 0;

int tproxy_setup_redirect_ipv4(atp_config_t *cfg) {
    return tproxy_setup_redirect_family(cfg, 4);

int tproxy_setup_redirect_ipv6(atp_config_t *cfg) {
    return tproxy_setup_redirect_family(cfg, 6);

static int tproxy_setup_enhance_family(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!ctx->enabled(cfg)) {
        LOG_DEBUG("Family %d proxy disabled, skipping ENHANCE", family);
        return 0;

    if (!family_available(family)) {
        if (family == 6) {
            LOG_WARN("ip6tables not found, IPv6 ENHANCE skipped");
            cfg->network.proxy_ipv6 = 0;
            return 0;
        return -1;

    LOG_INFO("Setting up ENHANCE mode for family %d", family);

    char redirect_tcp[64], udp_tproxy[64];
    build_chain_name(family, "REDIRECT_TCP", redirect_tcp, sizeof(redirect_tcp));
    build_chain_name(family, "UDP_TPROXY", udp_tproxy, sizeof(udp_tproxy));
    if (redirect_tcp[0] == '\0' || udp_tproxy[0] == '\0') return -1;

    int mark = ctx->mark(cfg);
    char rule_buf[256];

    tproxy_chain_create(cfg, family, "nat", redirect_tcp);
    tproxy_chain_flush(cfg, family, "nat", redirect_tcp);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j REDIRECT --to-ports %d",
             cfg->network.redirect_tcp_port);
    tproxy_rule_ensure_single(cfg, family, "nat", redirect_tcp, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p tcp -j %s", redirect_tcp);
    tproxy_rule_ensure_single_insert(cfg, family, "nat", "PREROUTING", 1, rule_buf);
    tproxy_rule_ensure_single_insert(cfg, family, "nat", "OUTPUT", 1, rule_buf);

    tproxy_chain_create(cfg, family, "mangle", udp_tproxy);
    tproxy_chain_flush(cfg, family, "mangle", udp_tproxy);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p udp -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.udp_port, mark);
    tproxy_rule_ensure_single(cfg, family, "mangle", udp_tproxy, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j %s", udp_tproxy);
    tproxy_rule_ensure_single_insert(cfg, family, "mangle", "PREROUTING", 1, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", mark);
    tproxy_rule_ensure_single_insert(cfg, family, "mangle", "OUTPUT", 1, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             mark, cfg->network.redirect_tcp_port);
    tproxy_rule_ensure_single(cfg, family, "nat", "PREROUTING", rule_buf);
    tproxy_rule_ensure_single(cfg, family, "nat", "OUTPUT", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core.core_user, cfg->core.core_group);
    tproxy_rule_ensure_single_insert(cfg, family, "nat", "OUTPUT", 1, rule_buf);

    LOG_INFO("ENHANCE setup complete for family %d", family);
    return 0;

int tproxy_setup_enhance_ipv4(atp_config_t *cfg) {
    return tproxy_setup_enhance_family(cfg, 4);

int tproxy_setup_enhance_ipv6(atp_config_t *cfg) {
    return tproxy_setup_enhance_family(cfg, 6);

static int tproxy_cleanup_owner_rule(atp_config_t *cfg, int family) {
    char hook_rule[128];
    SAFE_SNPRINTF(hook_rule, sizeof(hook_rule),
             "-m owner --uid-owner %s --gid-owner %s -j RETURN",
             cfg->core.core_user, cfg->core.core_group);
    delete_all_rules(cfg, family, "mangle", "OUTPUT", hook_rule);
    return 0;

static int tproxy_cleanup_family(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!ctx->enabled(cfg)) return 0;

    if (!family_available(family)) {
        LOG_DEBUG("Family %d not available, skipping cleanup", family);
        return 0;

    LOG_INFO("Cleaning up TPROXY chains for family %d", family);

    char pre_chain[64], out_chain[64];
    build_chain_name(family, "PRE_0", pre_chain, sizeof(pre_chain));
    build_chain_name(family, "OUT_0", out_chain, sizeof(out_chain));
    if (pre_chain[0] == '\0' || out_chain[0] == '\0') return -1;

    int mark = ctx->mark(cfg);

    tproxy_cleanup_owner_rule(cfg, family);

    char rule_buf[256];
    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", pre_chain);
    delete_all_rules(cfg, family, "mangle", "PREROUTING", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", out_chain);
    delete_all_rules(cfg, family, "mangle", "OUTPUT", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-m connmark --mark %d/0xff -j TPROXY --on-port %d --tproxy-mark %d",
             mark, cfg->network.tcp_port, mark);
    delete_all_rules(cfg, family, "mangle", "PREROUTING", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-m connmark --mark %d/0xff -j MARK --set-mark %d",
             mark, mark);
    delete_all_rules(cfg, family, "mangle", "OUTPUT", rule_buf);

    for (int i = 0; chain_suffixes[i] != NULL; i++) {
        char chain_name_buf[64];
        build_chain_name(family, chain_suffixes[i], chain_name_buf, sizeof(chain_name_buf));
        if (chain_name_buf[0] == '\0') continue;
        tproxy_chain_flush(cfg, family, "mangle", chain_name_buf);
        tproxy_chain_destroy(cfg, family, "mangle", chain_name_buf);

    LOG_INFO("TPROXY cleanup complete for family %d", family);
    return 0;

int tproxy_cleanup_ipv4(atp_config_t *cfg) {
    return tproxy_cleanup_family(cfg, 4);

int tproxy_cleanup_ipv6(atp_config_t *cfg) {
    return tproxy_cleanup_family(cfg, 6);

static int tproxy_cleanup_redirect_family(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!ctx->enabled(cfg)) return 0;

    LOG_INFO("Cleaning up REDIRECT chains for family %d", family);

    char chain[64];
    build_chain_name(family, "REDIRECT", chain, sizeof(chain));
    if (chain[0] == '\0') return -1;

    char rule_buf[256];

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", chain);
    delete_all_rules(cfg, family, "nat", "PREROUTING", rule_buf);
    delete_all_rules(cfg, family, "nat", "OUTPUT", rule_buf);

    tproxy_chain_flush(cfg, family, "nat", chain);
    tproxy_chain_destroy(cfg, family, "nat", chain);

    LOG_INFO("REDIRECT cleanup complete for family %d", family);
    return 0;

int tproxy_cleanup_redirect_ipv4(atp_config_t *cfg) {
    return tproxy_cleanup_redirect_family(cfg, 4);

int tproxy_cleanup_redirect_ipv6(atp_config_t *cfg) {
    return tproxy_cleanup_redirect_family(cfg, 6);

static int tproxy_cleanup_enhance_family(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    if (!ctx->enabled(cfg)) return 0;

    LOG_INFO("Cleaning up ENHANCE chains for family %d", family);

    char redirect_tcp[64], udp_tproxy[64];
    build_chain_name(family, "REDIRECT_TCP", redirect_tcp, sizeof(redirect_tcp));
    build_chain_name(family, "UDP_TPROXY", udp_tproxy, sizeof(udp_tproxy));
    if (redirect_tcp[0] == '\0' || udp_tproxy[0] == '\0') return -1;

    int mark = ctx->mark(cfg);
    char rule_buf[256];

    char tcp_rule[256];
    SAFE_SNPRINTF(tcp_rule, sizeof(tcp_rule), "-p tcp -j %s", redirect_tcp);
    delete_all_rules(cfg, family, "nat", "PREROUTING", tcp_rule);
    delete_all_rules(cfg, family, "nat", "OUTPUT", tcp_rule);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p tcp -m connmark --mark %d/0xff -j REDIRECT --to-ports %d",
             mark, cfg->network.redirect_tcp_port);
    delete_all_rules(cfg, family, "nat", "PREROUTING", rule_buf);
    delete_all_rules(cfg, family, "nat", "OUTPUT", rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p tcp -m owner --uid-owner %s --gid-owner %s -j ACCEPT",
             cfg->core.core_user, cfg->core.core_group);
    delete_all_rules(cfg, family, "nat", "OUTPUT", rule_buf);

    tproxy_chain_flush(cfg, family, "nat", redirect_tcp);
    tproxy_chain_destroy(cfg, family, "nat", redirect_tcp);

    char udp_rule[256];
    SAFE_SNPRINTF(udp_rule, sizeof(udp_rule), "-p udp -j %s", udp_tproxy);
    delete_all_rules(cfg, family, "mangle", "PREROUTING", udp_rule);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-p udp -j MARK --set-mark %d", mark);
    delete_all_rules(cfg, family, "mangle", "OUTPUT", rule_buf);

    tproxy_chain_flush(cfg, family, "mangle", udp_tproxy);
    tproxy_chain_destroy(cfg, family, "mangle", udp_tproxy);

    LOG_INFO("ENHANCE cleanup complete for family %d", family);
    return 0;

int tproxy_cleanup_enhance_ipv4(atp_config_t *cfg) {
    return tproxy_cleanup_enhance_family(cfg, 4);

int tproxy_cleanup_enhance_ipv6(atp_config_t *cfg) {
    return tproxy_cleanup_enhance_family(cfg, 6);

int tproxy_cleanup_xfrm_bypass(atp_config_t *cfg) {
    LOG_INFO("Cleaning up XFRM bypass chains");

    delete_all_rules(cfg, 4, "mangle", "PREROUTING", "-j XFRM_BYPASS");
    delete_all_rules(cfg, 4, "mangle", "OUTPUT", "-j XFRM_BYPASS");
    tproxy_chain_flush(cfg, 4, "mangle", "XFRM_BYPASS");
    tproxy_chain_destroy(cfg, 4, "mangle", "XFRM_BYPASS");

    delete_all_rules(cfg, 4, "nat", "PREROUTING", "-j XFRM_BYPASS_NAT");
    delete_all_rules(cfg, 4, "nat", "OUTPUT", "-j XFRM_BYPASS_NAT");
    tproxy_chain_flush(cfg, 4, "nat", "XFRM_BYPASS_NAT");
    tproxy_chain_destroy(cfg, 4, "nat", "XFRM_BYPASS_NAT");

    LOG_INFO("XFRM bypass chains cleaned up");
    return 0;

typedef struct {
    char table[32];
    char chain[64];

static int parse_iptables_save_for_orphans(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) return -1;

    if (cfg->core.dry_run) {
        LOG_DEBUG("[DRY_RUN] skipping orphan cleanup");
        return 0;

    if (!family_available(family)) return 0;

    char cmd[MAX_CMD_LEN];
    int n = snprintf(cmd, sizeof(cmd), "%s 2>/dev/null", ctx->save_cmd);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        LOG_ERROR("Command truncated");
        return -1;

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERROR("Failed to execute %s", ctx->save_cmd);
        return -1;

    char line[1024];
    char current_table[64] = {0};
    orphan_chain_t chains[MAX_ORPHAN_CHAINS];
    int chain_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';

        if (line[0] == '*') {
            snprintf(current_table, sizeof(current_table), "%s", line + 1);
            continue;

        if (line[0] == ':' &&
            (strncmp(line + 1, "ATP", 3) == 0 ||
             strncmp(line + 1, "XFRM", 4) == 0)) {

            char *space = strchr(line, ' ');
            if (space) {
                *space = '\0';
                char chain_name[64];
                SAFE_SNPRINTF(chain_name, sizeof(chain_name), "%s", line + 1);

                if (is_known_atp_chain(family, chain_name)) {
                    if (chain_count < MAX_ORPHAN_CHAINS) {
                        SAFE_SNPRINTF(chains[chain_count].table, sizeof(chains[0].table),
                                "%s", current_table);
                        SAFE_SNPRINTF(chains[chain_count].chain, sizeof(chains[0].chain),
                                "%s", chain_name);
                        chain_count++;
                        LOG_WARN("Too many orphan chains, max %d", MAX_ORPHAN_CHAINS);
            continue;

        if (strcmp(line, "COMMIT") == 0) {
            current_table[0] = '\0';

    int ret = pclose(fp);
    int exit_status = 0;
    if (WIFEXITED(ret)) {
        exit_status = WEXITSTATUS(ret);
        LOG_ERROR("%s killed by signal %d", ctx->save_cmd, WTERMSIG(ret));
        exit_status = -1;

    if (chain_count == 0) {
        return exit_status;

    /* Stage 1: Remove builtin references */
    for (int i = 0; i < chain_count; i++) {
        char rule_buf[256];
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-j %s", chains[i].chain);

        delete_all_rules(cfg, family, chains[i].table, "PREROUTING", rule_buf);
        delete_all_rules(cfg, family, chains[i].table, "OUTPUT", rule_buf);
        delete_all_rules(cfg, family, chains[i].table, "INPUT", rule_buf);
        delete_all_rules(cfg, family, chains[i].table, "FORWARD", rule_buf);

    /* Stage 2: Flush all ATP chains */
    for (int i = 0; i < chain_count; i++) {
        exec_ipt_flush_fast(cfg, family, chains[i].table, chains[i].chain);

    /* Stage 3: Destroy all ATP chains */
    for (int i = 0; i < chain_count; i++) {
        exec_ipt_destroy_fast(cfg, family, chains[i].table, chains[i].chain);
        LOG_DEBUG("Removed orphan chain: %s from table %s", chains[i].chain, chains[i].table);

    return exit_status;

int tproxy_cleanup_orphan_chains(atp_config_t *cfg) {
    LOG_INFO("Cleaning up orphan ATP chains");

    int ret = 0;
    ret |= parse_iptables_save_for_orphans(cfg, 4);
    if (cfg->network.proxy_ipv6 && family_available(6)) {
        ret |= parse_iptables_save_for_orphans(cfg, 6);

    return ret;

int tproxy_cleanup_all(atp_config_t *cfg) {
    LOG_INFO("Starting full cleanup of all ATP rules");

    tproxy_dns_hijack_cleanup(cfg, 4);
    if (cfg->network.proxy_ipv6) tproxy_dns_hijack_cleanup(cfg, 6);

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

    // Deep cleanup always enabled
    tproxy_cleanup_orphan_chains(cfg);
        tproxy_cleanup_orphan_chains(cfg);

    LOG_INFO("Full cleanup completed");
    return 0;

int tproxy_restart(atp_config_t *cfg) {
    LOG_INFO("Starting TPROXY restart (stop + start)");

    tproxy_cleanup_all(cfg);

    if (!tproxy_support_check(cfg)) {
        LOG_ERROR("TPROXY not supported on this kernel");
        return -1;

    int ret = 0;
    switch (cfg->network.proxy_mode) {
        case MODE_TPROXY:
            ret = tproxy_setup_ipv4(cfg);
            if (ret == 0 && cfg->network.proxy_ipv6) ret = tproxy_setup_ipv6(cfg);
            break;
        case MODE_REDIRECT:
            ret = tproxy_setup_redirect_ipv4(cfg);
            if (ret == 0 && cfg->network.proxy_ipv6) ret = tproxy_setup_redirect_ipv6(cfg);
            break;
        case MODE_ENHANCE:
            ret = tproxy_setup_enhance_ipv4(cfg);
            if (ret == 0 && cfg->network.proxy_ipv6) ret = tproxy_setup_enhance_ipv6(cfg);
            break;

    if (ret != 0) {
        LOG_ERROR("Failed to setup TPROXY rules");
        return ret;

    if (cfg->network.dns_hijack != DNS_HIJACK_OFF) {
        tproxy_dns_hijack_setup(cfg, 4, cfg->network.dns_hijack);
        if (cfg->network.proxy_ipv6) tproxy_dns_hijack_setup(cfg, 6, cfg->network.dns_hijack);

    if (cfg->core.block_quic) tproxy_block_quic(cfg, 1);
    if (cfg->network.loopback_protect) tproxy_block_loopback(cfg, 1);

    tproxy_prevent_loop(cfg);
    tproxy_xfrm_bypass(cfg);

    LOG_INFO("TPROXY restart completed successfully");
    return 0;

int tproxy_dns_hijack_setup(atp_config_t *cfg, int family, int mode) {
    if (cfg->network.dns_hijack == DNS_HIJACK_OFF || mode == DNS_HIJACK_OFF) return 0;

    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    char dns_pre[64], dns_out[64];
    build_chain_name(family, "DNS_PRE_0", dns_pre, sizeof(dns_pre));
    build_chain_name(family, "DNS_OUT_0", dns_out, sizeof(dns_out));
    if (dns_pre[0] == '\0' || dns_out[0] == '\0') return -1;

    int mark = ctx->mark(cfg);
    char rule_buf[128];

    if (mode == DNS_HIJACK_TPROXY) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
                 cfg->network.dns_port, mark);
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-p udp --dport 53 -j REDIRECT --to-ports %d",
                 cfg->network.dns_port);
        return 0;

    tproxy_rule_ensure_single(cfg, family, "mangle", dns_pre, rule_buf);
    tproxy_rule_ensure_single(cfg, family, "mangle", dns_out, rule_buf);

    return 0;

int tproxy_dns_hijack_cleanup(atp_config_t *cfg, int family) {
    const tproxy_family_ctx_t *ctx = get_ctx(family);
    if (!ctx) {
        LOG_ERROR("Invalid family: %d", family);
        return -1;

    char dns_pre[64], dns_out[64];
    build_chain_name(family, "DNS_PRE_0", dns_pre, sizeof(dns_pre));
    build_chain_name(family, "DNS_OUT_0", dns_out, sizeof(dns_out));
    if (dns_pre[0] == '\0' || dns_out[0] == '\0') return -1;

    int mark = ctx->mark(cfg);
    char rule_buf[128];

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p udp --dport 53 -j TPROXY --on-port %d --tproxy-mark %d",
             cfg->network.dns_port, mark);
    delete_all_rules(cfg, family, "mangle", dns_pre, rule_buf);
    delete_all_rules(cfg, family, "mangle", dns_out, rule_buf);

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
             "-p udp --dport 53 -j REDIRECT --to-ports %d",
             cfg->network.dns_port);
    delete_all_rules(cfg, family, "mangle", dns_pre, rule_buf);
    delete_all_rules(cfg, family, "mangle", dns_out, rule_buf);

    return 0;

static int tproxy_reject_or_drop(atp_config_t *cfg, int family, const char *chain, const char *rule) {
    char modified_rule[256];
    if (tproxy_reject_available()) {
        SAFE_SNPRINTF(modified_rule, sizeof(modified_rule), "%s -j REJECT", rule);
        SAFE_SNPRINTF(modified_rule, sizeof(modified_rule), "%s -j DROP", rule);
    return tproxy_rule_ensure_single(cfg, family, "filter", chain, modified_rule);

int tproxy_block_quic(atp_config_t *cfg, int enable) {
    if (enable) {
        LOG_INFO("Enabling QUIC blocking");

        tproxy_chain_create(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");

        if (cfg->ebpf.ready) {
            const char *pin_dir = boxbpf_pin_dir();
            char bpf_rule[512];
            SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                     "-m bpf --object-pinned %s/box_cidr_out4 -p udp --dport 443 -j REJECT", pin_dir);
            tproxy_rule_ensure_single(cfg, 4, "filter", "ATP_QUIC_0", bpf_rule);
            LOG_DEBUG("QUIC blocking: eBPF");
            tproxy_reject_or_drop(cfg, 4, "ATP_QUIC_0", "-p udp --dport 443");
            LOG_DEBUG("QUIC blocking: ipset fallback");

        tproxy_rule_ensure_single(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        tproxy_rule_ensure_single(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        tproxy_rule_ensure_single(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");

        if (cfg->network.proxy_ipv6 && family_available(6)) {
            tproxy_chain_create(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");

            if (cfg->ebpf.ready) {
                const char *pin_dir = boxbpf_pin_dir();
                char bpf_rule[512];
                SAFE_SNPRINTF(bpf_rule, sizeof(bpf_rule),
                         "-m bpf --object-pinned %s/box_cidr_out6 -p udp --dport 443 -j REJECT", pin_dir);
                tproxy_rule_ensure_single(cfg, 6, "filter", "ATP6_QUIC_0", bpf_rule);
                tproxy_reject_or_drop(cfg, 6, "ATP6_QUIC_0", "-p udp --dport 443");

            tproxy_rule_ensure_single(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            tproxy_rule_ensure_single(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            tproxy_rule_ensure_single(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
        LOG_INFO("Disabling QUIC blocking");

        delete_all_rules(cfg, 4, "filter", "INPUT", "-j ATP_QUIC_0");
        delete_all_rules(cfg, 4, "filter", "FORWARD", "-j ATP_QUIC_0");
        delete_all_rules(cfg, 4, "filter", "OUTPUT", "-j ATP_QUIC_0");
        tproxy_chain_flush(cfg, 4, "filter", "ATP_QUIC_0");
        tproxy_chain_destroy(cfg, 4, "filter", "ATP_QUIC_0");

        if (cfg->network.proxy_ipv6 && family_available(6)) {
            delete_all_rules(cfg, 6, "filter", "INPUT", "-j ATP6_QUIC_0");
            delete_all_rules(cfg, 6, "filter", "FORWARD", "-j ATP6_QUIC_0");
            delete_all_rules(cfg, 6, "filter", "OUTPUT", "-j ATP6_QUIC_0");
            tproxy_chain_flush(cfg, 6, "filter", "ATP6_QUIC_0");
            tproxy_chain_destroy(cfg, 6, "filter", "ATP6_QUIC_0");

    return 0;

int tproxy_block_loopback(atp_config_t *cfg, int enable) {
    char rule_buf[256];

    if (enable) {
        LOG_INFO("Enabling loopback protection");
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT", cfg->network.tcp_port);
        tproxy_rule_ensure_single(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->network.proxy_ipv6 && family_available(6)) {
            SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT", cfg->network.tcp_port);
            tproxy_rule_ensure_single(cfg, 6, "filter", "OUTPUT", rule_buf);
        LOG_INFO("Disabling loopback protection");
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                 "-d 127.0.0.1 -p tcp -m tcp --dport %d -j REJECT", cfg->network.tcp_port);
        delete_all_rules(cfg, 4, "filter", "OUTPUT", rule_buf);

        if (cfg->network.proxy_ipv6 && family_available(6)) {
            SAFE_SNPRINTF(rule_buf, sizeof(rule_buf),
                     "-d ::1 -p tcp -m tcp --dport %d -j REJECT", cfg->network.tcp_port);
            delete_all_rules(cfg, 6, "filter", "OUTPUT", rule_buf);

    return 0;

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

int tproxy_prevent_loop(atp_config_t *cfg) {
    char rule_buf[64];

    SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->network.mark_value);
    delete_all_rules(cfg, 4, "mangle", "PREROUTING", rule_buf);
    tproxy_rule_insert(cfg, 4, "mangle", "PREROUTING", 1, rule_buf);

    if (cfg->network.proxy_ipv6 && family_available(6)) {
        SAFE_SNPRINTF(rule_buf, sizeof(rule_buf), "-m mark --mark %d -j RETURN", cfg->network.mark_value6);
        delete_all_rules(cfg, 6, "mangle", "PREROUTING", rule_buf);
        tproxy_rule_insert(cfg, 6, "mangle", "PREROUTING", 1, rule_buf);

    return 0;

int tproxy_sound_bypass(atp_config_t *cfg) {
    (void)cfg;
    return 0;
}
