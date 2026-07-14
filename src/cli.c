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
    {"ipv6",      required_argument, 0, '6'},
    {"config",    required_argument, 0, 'c'},
    {"help",      no_argument,       0, 'h'},
    {"version",   no_argument,       0, 'v'},
    {0, 0, 0, 0}
};

static const char *short_options = "c:p:fdqFtn6:hv";

static int parse_ebpf_command(int argc, char *argv[], atp_options_t *opts) {
    if (argc < 3) {
        fprintf(stderr, "ebpf: missing subcommand\n");
        fprintf(stderr, "Usage: atpd ebpf {probe|init|apply|update|clear|status}\n");
        return -1;
    }

    const char *sub = argv[2];
    if (strcmp(sub, "probe") == 0) {
        opts->command = CMD_EBPF_PROBE;
    } else if (strcmp(sub, "init") == 0) {
        opts->command = CMD_EBPF_INIT;
    } else if (strcmp(sub, "apply") == 0) {
        opts->command = CMD_EBPF_APPLY;
    } else if (strcmp(sub, "update") == 0) {
        opts->command = CMD_EBPF_UPDATE;
    } else if (strcmp(sub, "clear") == 0) {
        opts->command = CMD_EBPF_CLEAR;
    } else if (strcmp(sub, "status") == 0) {
        opts->command = CMD_EBPF_STATUS;
    } else {
        fprintf(stderr, "ebpf: unknown subcommand '%s'\n", sub);
        fprintf(stderr, "Usage: atpd ebpf {probe|init|apply|update|clear|status}\n");
        return -1;
    }

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--ipv6") == 0 && i + 1 < argc) {
            opts->ipv6 = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            strncpy(opts->ebpf_config, argv[i + 1], sizeof(opts->ebpf_config) - 1);
            i++;
        }
    }

    return 0;
}

void print_usage(const char *progname) {
    const char *base = strrchr(progname, '/');
    base = base ? base + 1 : progname;

    printf(ATP_NAME " v" ATP_VERSION_STRING "\n\n");
    printf("Usage: %s [options] command [subcommand] [args]\n\n", base);
    printf("Options:\n");
    printf("  -c, --config FILE     Specify configuration file\n");
    printf("  -p, --pid FILE        Specify PID file path\n");
    printf("  -d, --daemon          Run as daemon (default for start)\n");
    printf("  -f, --foreground      Run in foreground (do not daemonize)\n");
    printf("  -V, --verbose         Verbose output (debug level)\n");
    printf("  -q, --quiet           Quiet output (errors only)\n");
    printf("  -F, --force           Skip confirmation for dangerous operations\n");
    printf("  -t, --test            Test configuration and exit\n");
    printf("  -n, --no-color        Disable colored output\n");
    printf("  -6, --ipv6 1|0        Enable/disable IPv6 for eBPF probe\n");
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
    printf("\neBPF Commands (boxbpf compatibility):\n");
    printf("  ebpf probe [--ipv6 1] Detect kernel eBPF support\n");
    printf("  ebpf init --config FILE Initialize eBPF maps and programs\n");
    printf("  ebpf apply --config FILE Load eBPF programs and pin\n");
    printf("  ebpf update --config FILE Hot update CIDR/UID maps\n");
    printf("  ebpf clear              Remove all eBPF pin files\n");
    printf("  ebpf status             Show eBPF status\n");
    printf("\nExamples:\n");
    printf("  %s status\n", base);
    printf("  %s ebpf probe --ipv6 1\n", base);
    printf("  %s ebpf clear\n", base);
}

void print_version(void) {
    printf("atpd %s\n", ATP_VERSION_STRING);
}

void print_help(const char *progname) {
    print_usage(progname);
}

static const char* suggest_command(const char *cmd) {
    const char *commands[] = {"start", "stop", "restart", "status",
                              "reload", "check", "update-geoip",
                              "version", "help", "ebpf", NULL};
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
    opts->ipv6 = 1;

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
            case '6':
                opts->ipv6 = atoi(optarg);
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
        else if (strcmp(cmd, "ebpf") == 0) {
            if (parse_ebpf_command(argc, argv, opts) != 0) {
                return -1;
            }
        } else {
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
        case CMD_EBPF_PROBE:   return "ebpf probe";
        case CMD_EBPF_INIT:    return "ebpf init";
        case CMD_EBPF_APPLY:   return "ebpf apply";
        case CMD_EBPF_UPDATE:  return "ebpf update";
        case CMD_EBPF_CLEAR:   return "ebpf clear";
        case CMD_EBPF_STATUS:  return "ebpf status";
        default:               return "unknown";
    }
}
