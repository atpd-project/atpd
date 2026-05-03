/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2025 ATP Project
 *
 * Command line interface implementation
 */

#include "cli.h"
#include "logger.h"
#include "atp.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "atp.h"

#define ATPD_SOCK_PATH ATP_DEFAULT_DIR "/run/atpd.sock"

static const struct option long_options[] = {
    {"config",    required_argument, 0, 'c'},
    {"pid",       required_argument, 0, 'p'},
    {"foreground",no_argument,       0, 'f'},
    {"daemon",    no_argument,       0, 'd'},
    {"verbose",   no_argument,       0, 'V'},
    {"quiet",     no_argument,       0, 'q'},
    {"force",     no_argument,       0, 'F'},
    {"test",      no_argument,       0, 't'},
    {"no-color",  no_argument,       0, 'n'},
    {"help",      no_argument,       0, 'h'},
    {"version",   no_argument,       0, 'v'},
    {0, 0, 0, 0}
};

static const char *short_options = "c:p:fdqFtnhv";

void print_usage(const char *progname) {
    const char *base = strrchr(progname, '/');
    base = base ? base + 1 : progname;

    printf(ATP_NAME " v" ATP_VERSION_STRING "\n\n");
    printf("Usage: %s [options] command\n\n", base);
    printf("Options:\n");
    printf("  -c, --config FILE     Specify configuration file\n");
    printf("  -p, --pid FILE        Specify PID file path\n");
    printf("  -d, --daemon          Run as daemon (default for start)\n");
    printf("  -f, --foreground      Run in foreground (do not daemonize)\n");
    printf("  --verbose             Verbose output (debug level)\n");
    printf("  -q, --quiet           Quiet output (errors only)\n");
    printf("  --force               Skip confirmation for dangerous operations\n");
    printf("  -t                    Test configuration and exit\n");
    printf("  -n, --no-color        Disable colored output\n");
    printf("  -h, --help            Show this help\n");
    printf("  -v, --version         Print version and exit\n");
    printf("\nCommands:\n");
    printf("  start                 Start daemon\n");
    printf("  stop                  Stop daemon\n");
    printf("  restart               Restart daemon\n");
    printf("  status                Show daemon status and statistics\n");
    printf("  reload                Reload configuration without restart\n");
    printf("  check                 Validate configuration and exit\n");
    printf("  update-geoip          Update GeoIP database\n");
    printf("  version               Print version information\n");
    printf("  help                  Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s status                            # Show daemon status\n", base);
    printf("  %s -c atp.conf start                 # Start with custom config\n", base);
    printf("  %s -f --verbose start                # Foreground with debug log\n", base);
    printf("  %s -c atp.conf -p atpd.pid start     # Custom config and PID file\n", base);
    printf("  %s -t                                # Test configuration\n", base);
    printf("  %s stop --force                      # Force stop without prompt\n", base);
}

void print_version(void) {
    printf("atpd %s\n", ATP_VERSION_STRING);
}

void print_help(const char *progname) {
    print_usage(progname);
}

/* Find the most similar command for suggestions */
static const char* suggest_command(const char *cmd) {
    const char *commands[] = {"start", "stop", "restart", "status",
                              "reload", "check", "update-geoip",
                              "version", "help", NULL};
    for (int i = 0; commands[i]; i++) {
        if (strncmp(cmd, commands[i], strlen(cmd)) == 0) {
            return commands[i];
        }
    }
    return NULL;
}

int parse_arguments(int argc, char *argv[], atp_options_t *opts) {
    memset(opts, 0, sizeof(atp_options_t));
    opts->command = CMD_NONE;
    opts->daemon = 1;
    opts->log_level = LOG_LEVEL_INFO;
    
    int opt;
    int option_index = 0;
    
    optind = 1;
    opterr = 1;
    
    while ((opt = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c':
                strncpy(opts->config_file, optarg, sizeof(opts->config_file) - 1);
                opts->config_file[sizeof(opts->config_file) - 1] = '\0';
                break;
            case 'p':
                strncpy(opts->pid_file, optarg, sizeof(opts->pid_file) - 1);
                opts->pid_file[sizeof(opts->pid_file) - 1] = '\0';
                break;
            case 'f':
                opts->foreground = 1;
                opts->daemon = 0;
                break;
            case 'd':
                opts->daemon = 1;
                opts->foreground = 0;
                break;
            case 'V':
                opts->verbose = 1;
                opts->log_level = LOG_LEVEL_DEBUG;
                break;
            case 'q':
                opts->quiet = 1;
                opts->log_level = LOG_LEVEL_ERROR;
                break;
            case 'F':
                opts->force = 1;
                break;
            case 't':
                opts->test_config = 1;
                opts->command = CMD_CHECK;
                break;
            case 'n':
                opts->no_color = 1;
                break;
            case 'h':
                opts->command = CMD_HELP;
                break;
            case 'v':
                opts->command = CMD_VERSION;
                break;
            case '?':
                return -1;
            default:
                break;
        }
    }
    
    if (opts->command == CMD_NONE && optind < argc) {
        const char *cmd = argv[optind];
        if (strcmp(cmd, "start") == 0) opts->command = CMD_START;
        else if (strcmp(cmd, "stop") == 0) opts->command = CMD_STOP;
        else if (strcmp(cmd, "restart") == 0) opts->command = CMD_RESTART;
        else if (strcmp(cmd, "status") == 0) opts->command = CMD_STATUS;
        else if (strcmp(cmd, "update-geoip") == 0) opts->command = CMD_UPDATE_GEOIP;
        else if (strcmp(cmd, "reload") == 0) opts->command = CMD_RELOAD;
        else if (strcmp(cmd, "check") == 0) opts->command = CMD_CHECK;
        else if (strcmp(cmd, "help") == 0) opts->command = CMD_HELP;
        else if (strcmp(cmd, "version") == 0) opts->command = CMD_VERSION;
        else {
            const char *suggestion = suggest_command(cmd);
            if (suggestion) {
                fprintf(stderr, "atpd: unknown command '%s'. Did you mean '%s'?\n", cmd, suggestion);
            } else {
                fprintf(stderr, "atpd: unknown command '%s'\n", cmd);
            }
            fprintf(stderr, "Try 'atpd --help' for all commands.\n");
            return -1;
        }
    }
    
    if (opts->command == CMD_NONE) {
        opts->command = CMD_HELP;
    }
    
    return 0;
}

const char* command_to_string(atp_command_t cmd) {
    switch (cmd) {
        case CMD_START:        return "start";
        case CMD_STOP:         return "stop";
        case CMD_RESTART:      return "restart";
        case CMD_STATUS:       return "status";
        case CMD_UPDATE_GEOIP: return "update-geoip";
        case CMD_RELOAD:       return "reload";
        case CMD_CHECK:        return "check";
        case CMD_VERSION:      return "version";
        case CMD_HELP:         return "help";
        default:               return "unknown";
    }
}
