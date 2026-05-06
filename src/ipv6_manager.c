/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * IPv6 Manager - Proc sysctl interface
 */

#include "ipv6_manager.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define PROC_IPV6_CONF "/proc/sys/net/ipv6/conf"

static int ipv6_write_sysctl(atp_config_t *cfg, const char *path, int value) {
    if (cfg && cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would write %d to %s", value, path);
        return 0;
    }
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_DEBUG("Failed to write to %s: %s", path, strerror(errno));
        return -1;
    }
    fprintf(fp, "%d\n", value);
    fclose(fp);
    return 0;
}

static int ipv6_read_sysctl(const char *path, int *value) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (fscanf(fp, "%d", value) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

int ipv6_manager_backup(atp_config_t *cfg, ipv6_backup_t *backup) {
    memset(backup, 0, sizeof(ipv6_backup_t));

    if (cfg && cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would backup IPv6 sysctl settings");
        backup->backup_exists = 1;
        return 0;
    }

    ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/accept_ra", &backup->original_accept_ra);
    ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/autoconf", &backup->original_autoconf);
    ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", &backup->original_forwarding);

    backup->backup_exists = 1;

    LOG_DEBUG("IPv6 backup: accept_ra=%d, autoconf=%d, forwarding=%d",
              backup->original_accept_ra, backup->original_autoconf, backup->original_forwarding);

    return 0;
}

int ipv6_manager_restore(atp_config_t *cfg, ipv6_backup_t *backup) {
    if (!backup->backup_exists) {
        LOG_DEBUG("No IPv6 backup to restore");
        return 0;
    }

    if (cfg && cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would restore IPv6 sysctl settings");
        backup->backup_exists = 0;
        return 0;
    }

    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/accept_ra", backup->original_accept_ra);
    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/autoconf", backup->original_autoconf);
    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/forwarding", backup->original_forwarding);

    LOG_DEBUG("IPv6 settings restored: accept_ra=%d, autoconf=%d, forwarding=%d",
              backup->original_accept_ra, backup->original_autoconf, backup->original_forwarding);

    backup->backup_exists = 0;

    return 0;
}

int ipv6_manager_disable_all(atp_config_t *cfg) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    int disabled_count = 0;

    if (cfg && cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would disable IPv6 on all interfaces");
        return 0;
    }

    LOG_INFO("Disabling IPv6 on all interfaces");

    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/forwarding", 0);
    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/accept_ra", 0);
    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/autoconf", 0);

    dir = opendir(PROC_IPV6_CONF);
    if (!dir) {
        LOG_ERROR("Failed to open %s", PROC_IPV6_CONF);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        snprintf(path, sizeof(path), "%s/%s/disable_ipv6", PROC_IPV6_CONF, entry->d_name);
        if (ipv6_write_sysctl(cfg, path, 1) == 0) {
            disabled_count++;
        }
    }

    closedir(dir);
    LOG_INFO("IPv6 disabled on %d interfaces", disabled_count);
    return 0;
}

int ipv6_manager_enable_all(atp_config_t *cfg) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    int enabled_count = 0;

    if (cfg && cfg->dry_run) {
        LOG_DEBUG("[DRY_RUN] Would enable IPv6 on all interfaces");
        return 0;
    }

    LOG_INFO("Enabling IPv6 on all interfaces");

    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/forwarding", 1);
    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/accept_ra", 1);
    ipv6_write_sysctl(cfg, "/proc/sys/net/ipv6/conf/all/autoconf", 1);

    dir = opendir(PROC_IPV6_CONF);
    if (!dir) {
        LOG_ERROR("Failed to open %s", PROC_IPV6_CONF);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        snprintf(path, sizeof(path), "%s/%s/disable_ipv6", PROC_IPV6_CONF, entry->d_name);
        if (ipv6_write_sysctl(cfg, path, 0) == 0) {
            enabled_count++;
        }
    }

    closedir(dir);
    LOG_INFO("IPv6 enabled on %d interfaces", enabled_count);
    return 0;
}

int ipv6_manager_is_disabled(void) {
    int disabled;
    if (ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/disable_ipv6", &disabled) < 0) {
        return 0;
    }
    return disabled;
}

int ipv6_manager_set_mode(atp_config_t *cfg, int mode) {
    if (mode == IPV6_MODE_DISABLED) {
        LOG_INFO("IPv6 mode: COMPLETELY DISABLED");
        return ipv6_manager_disable_all(cfg);
    } else if (mode == IPV6_MODE_PROXY) {
        LOG_INFO("IPv6 mode: PROXY ENABLED");
        if (ipv6_manager_is_disabled()) {
            ipv6_manager_enable_all(cfg);
        }
        return 0;
    } else {
        LOG_INFO("IPv6 mode: SYSTEM DEFAULT");
        if (ipv6_manager_is_disabled()) {
            ipv6_manager_enable_all(cfg);
        }
        return 0;
    }
}

int ipv6_manager_init(atp_config_t *cfg) {
    int mode;
    if (cfg->proxy_ipv6 == -1) {
        mode = IPV6_MODE_DISABLED;
    } else if (cfg->proxy_ipv6 == 1) {
        mode = IPV6_MODE_PROXY;
    } else {
        mode = IPV6_MODE_DEFAULT;
    }
    return ipv6_manager_set_mode(cfg, mode);
}
