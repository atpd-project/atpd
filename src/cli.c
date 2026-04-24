extern const char* atp_get_version(void);
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

static const char *copyright =
    ATP_NAME " v" ATP_VERSION_STRING "\n"
    "Copyright (C) 2024-2025 ATP Project\n"
    "License: GPL v3\n";

static const struct option long_options[] = {
    {"config",     required_argument, 0, 'c'},
    {"foreground", no_argument,       0, 'f'},
    {"daemon",     no_argument,       0, 'd'},
    {"verbose",    no_argument,       0, 'v'},
    {"quiet",      no_argument,       0, 'q'},
    {"force",      no_argument,       0, 'F'},
    {"no-color",   no_argument,       0, 'n'},
    {"help",       no_argument,       0, 'h'},
    {"version",    no_argument,       0, 'V'},
    {0, 0, 0, 0}
};

static const char *short_options = "c:fdvqFtnhV";

void print_usage(const char *progname) {
    const char *base = strrchr(progname, '/');
    base = base ? base + 1 : progname;

    printf(ATP_NAME " v" ATP_VERSION_STRING "\n\n");
    printf("Usage: %s [options] command\n\n", base);
    printf("Options:\n");
    printf("  -c, --config FILE   Specify configuration file\n");
    printf("  -t                  Test configuration and exit (same as 'check')\n");
    printf("  -d, --daemon        Run as daemon (default for start)\n");
    printf("  -f, --foreground    Run in foreground (do not daemonize)\n");
    printf("  -v, --verbose       Verbose output (debug level)\n");
    printf("  -q, --quiet         Quiet output (errors only)\n");
    printf("  --force             Skip confirmation for dangerous operations\n");
    printf("  --no-color          Disable colored output\n");
    printf("  -h, --help          Show this help\n");
    printf("  -V, --version       Show version\n");
    printf("\nCommands:\n");
    printf("  start               Start daemon\n");
    printf("  stop                Stop daemon\n");
    printf("  restart             Restart daemon\n");
    printf("  status              Show status\n");
    printf("  reload              Reload configuration\n");
    printf("  check               Check configuration syntax and validity\n");
    printf("  update-geoip        Update GeoIP database\n");
    printf("\nExamples:\n");
    printf("  %s status                       # Show status\n", base);
    printf("  %s -c atp.conf start            # Start with custom config\n", base);
    printf("  %s -f -v start                  # Start in foreground with verbose log\n", base);
    printf("  %s -t                           # Test configuration\n", base);
    printf("  %s stop --force                 # Stop without confirmation\n", base);
}

void print_version(void) {
    printf("atpd version %s\n", atp_get_version());
}

void print_help(const char *progname) {
    print_usage(progname);
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
            case 'f':
                opts->foreground = 1;
                opts->daemon = 0;
                break;
            case 'd':
                opts->daemon = 1;
                opts->foreground = 0;
                break;
            case 'v':
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
            case 'V':
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
            fprintf(stderr, "atpd: unknown command '%s'\n", cmd);
            fprintf(stderr, "Try 'atpd --help' for a list of available commands.\n");
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
