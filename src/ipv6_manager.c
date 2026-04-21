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
#define IPV6_BACKUP_FILE "run/ipv6_backup.conf"

static int ipv6_write_sysctl(const char *path, int value) {
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
    fscanf(fp, "%d", value);
    fclose(fp);
    return 0;
}

int ipv6_manager_backup(ipv6_backup_t *backup) {
    memset(backup, 0, sizeof(ipv6_backup_t));
    
    /* Read current settings */
    ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/accept_ra", &backup->original_accept_ra);
    ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/autoconf", &backup->original_autoconf);
    ipv6_read_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", &backup->original_forwarding);
    
    /* Mark backup as valid */
    backup->backup_exists = 1;
    
    LOG_DEBUG("IPv6 backup: accept_ra=%d, autoconf=%d, forwarding=%d",
              backup->original_accept_ra, backup->original_autoconf, backup->original_forwarding);
    
    return 0;
}

int ipv6_manager_restore(ipv6_backup_t *backup) {
    if (!backup->backup_exists) {
        LOG_DEBUG("No IPv6 backup to restore");
        return 0;
    }
    
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/accept_ra", backup->original_accept_ra);
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/autoconf", backup->original_autoconf);
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", backup->original_forwarding);
    
    LOG_DEBUG("IPv6 settings restored: accept_ra=%d, autoconf=%d, forwarding=%d",
              backup->original_accept_ra, backup->original_autoconf, backup->original_forwarding);
    
    /* Mark backup as no longer valid after restore */
    backup->backup_exists = 0;
    
    return 0;
}

int ipv6_manager_disable_all(void) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    int disabled_count = 0;
    
    LOG_INFO("Disabling IPv6 on all interfaces");
    
    /* Disable forwarding and RA for all interfaces */
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", 0);
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/accept_ra", 0);
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/autoconf", 0);
    
    /* Disable IPv6 on each interface */
    dir = opendir(PROC_IPV6_CONF);
    if (!dir) {
        LOG_ERROR("Failed to open %s", PROC_IPV6_CONF);
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        snprintf(path, sizeof(path), "%s/%s/disable_ipv6", PROC_IPV6_CONF, entry->d_name);
        if (ipv6_write_sysctl(path, 1) == 0) {
            disabled_count++;
        }
    }
    
    closedir(dir);
    LOG_INFO("IPv6 disabled on %d interfaces", disabled_count);
    return 0;
}

int ipv6_manager_enable_all(void) {
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    int enabled_count = 0;
    
    LOG_INFO("Enabling IPv6 on all interfaces");
    
    /* Restore forwarding and RA - use conservative defaults */
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", 1);
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/accept_ra", 1);
    ipv6_write_sysctl("/proc/sys/net/ipv6/conf/all/autoconf", 1);
    
    /* Enable IPv6 on each interface */
    dir = opendir(PROC_IPV6_CONF);
    if (!dir) {
        LOG_ERROR("Failed to open %s", PROC_IPV6_CONF);
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        snprintf(path, sizeof(path), "%s/%s/disable_ipv6", PROC_IPV6_CONF, entry->d_name);
        if (ipv6_write_sysctl(path, 0) == 0) {
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
	(void)cfg;
    if (mode == IPV6_MODE_DISABLED) {
        LOG_INFO("IPv6 mode: COMPLETELY DISABLED");
        return ipv6_manager_disable_all();
    } else if (mode == IPV6_MODE_PROXY) {
        LOG_INFO("IPv6 mode: PROXY ENABLED");
        if (ipv6_manager_is_disabled()) {
            ipv6_manager_enable_all();
        }
        return 0;
    } else {
        LOG_INFO("IPv6 mode: SYSTEM DEFAULT");
        if (ipv6_manager_is_disabled()) {
            ipv6_manager_enable_all();
        }
        return 0;
    }
}

int ipv6_manager_init(atp_config_t *cfg) {
    /* Convert proxy_ipv6 flag (0/1) to IPV6_MODE_* enum */
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
