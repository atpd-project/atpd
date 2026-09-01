/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Command line interface implementation
 */

#include "cli.h"
#include "version.h"
#include <stdio.h>
#include <string.h>

static const struct option long_options[] = {
    {"config",    required_argument, 0, 'c'},
    {"pid",       required_argument, 0, 'p'},
    {"foreground",no_argument,       0, 'f'},
    {"daemon",    no_argument,       0, 'd'},
    {"verbose",   no_argument,       0, 'V'},
    {"quiet",     no_argument,       0, 'q'},
    {"no-color",  no_argument,       0, 'n'},
    {"help",      no_argument,       0, 'h'},
    {"version",   no_argument,       0, 'v'},
    {0, 0, 0, 0}
};

static const char *short_options = ":c:p:fdVqnhv";

enum {
    OPTION_CONFIG = 1u << 0,
    OPTION_PID = 1u << 1,
    OPTION_FOREGROUND = 1u << 2,
    OPTION_DAEMON = 1u << 3,
    OPTION_VERBOSE = 1u << 4,
    OPTION_QUIET = 1u << 5,
    OPTION_NO_COLOR = 1u << 6,
    OPTION_HELP = 1u << 7,
    OPTION_VERSION = 1u << 8
};

static int copy_cli_path(char *dst, size_t dst_size, const char *src, const char *option_name) {
    size_t len;

    if (!dst || dst_size == 0 || !src) return -1;
    len = strlen(src);
    if (len >= dst_size) {
        fprintf(stderr, "%s: path too long\n", option_name);
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static int command_from_string(const char *name, atp_command_t *command) {
    static const struct {
        const char *name;
        atp_command_t command;
    } commands[] = {
        {"start", CMD_START}, {"stop", CMD_STOP}, {"restart", CMD_RESTART},
        {"status", CMD_STATUS}, {"reload", CMD_RELOAD}, {"check", CMD_CHECK},
        {"version", CMD_VERSION}, {"help", CMD_HELP}
    };

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(name, commands[i].name) == 0) {
            *command = commands[i].command;
            return 0;
        }
    }
    return -1;
}

static int validate_options(atp_command_t command, unsigned seen) {
    const unsigned mode = seen & (OPTION_FOREGROUND | OPTION_DAEMON);
    const unsigned verbosity = seen & (OPTION_VERBOSE | OPTION_QUIET);

    if (mode == (OPTION_FOREGROUND | OPTION_DAEMON)) {
        fprintf(stderr, "--foreground and --daemon are mutually exclusive\n");
        return -1;
    }
    if (verbosity == (OPTION_VERBOSE | OPTION_QUIET)) {
        fprintf(stderr, "--verbose and --quiet are mutually exclusive\n");
        return -1;
    }
    if (command != CMD_START && command != CMD_RESTART && mode) {
        fprintf(stderr, "run mode is only valid with start/restart\n");
        return -1;
    }
    if ((command == CMD_VERSION || command == CMD_HELP) && seen &
        (OPTION_CONFIG | OPTION_PID | OPTION_VERBOSE | OPTION_QUIET | OPTION_NO_COLOR)) {
        fprintf(stderr, "options are not valid with version/help\n");
        return -1;
    }
    if (command == CMD_CHECK && (seen & OPTION_PID)) {
        fprintf(stderr, "--pid is not valid with check\n");
        return -1;
    }
    if (seen & (OPTION_HELP | OPTION_VERSION)) {
        if (command != (seen & OPTION_HELP ? CMD_HELP : CMD_VERSION)) {
            fprintf(stderr, "help/version cannot be combined with a command\n");
            return -1;
        }
    }
    return 0;
}

void print_usage(const char *progname) {
    const char *base = progname ? strrchr(progname, '/') : NULL;
    base = base ? base + 1 : (progname ? progname : "atpd");

    printf("atpd %s\n\n", atp_get_full_version());
    printf("Usage: %s [options] command\n\n", base);
    printf("Options:\n");
    printf("  -c, --config FILE     Specify configuration file\n");
    printf("  -p, --pid FILE        Specify PID file path\n");
    printf("  -d, --daemon          Run as daemon (default for start)\n");
    printf("  -f, --foreground      Run in foreground (do not daemonize)\n");
    printf("  -V, --verbose         Verbose output (debug level)\n");
    printf("  -q, --quiet           Quiet output (errors only)\n");
    printf("  -n, --no-color        Disable colored output\n");
    printf("  -h, --help            Show this help\n");
    printf("  -v, --version         Print version and exit\n");
    printf("\nCommands:\n");
    printf("  start                 Start daemon\n");
    printf("  stop                  Stop daemon\n");
    printf("  restart               Restart daemon\n");
    printf("  status                Show daemon, VPN tunnel, and traffic status\n");
    printf("  reload                Reload configuration without restart\n");
    printf("  check                 Validate configuration and exit\n");
    printf("  help                  Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s status\n", base);
}

void print_version(void) {
    printf("atpd %s\n", atp_get_full_version());
}

void print_help(const char *progname) {
    print_usage(progname);
}

int parse_arguments(int argc, char *argv[], atp_options_t *opts) {
    if (!opts || argc < 0 || (argc > 0 && !argv)) return -1;
    memset(opts, 0, sizeof(atp_options_t));
    opts->command = CMD_NONE;
    opts->run_mode = CLI_RUN_MODE_DEFAULT;
    opts->verbosity = CLI_VERBOSITY_DEFAULT;

    int opt;
    int option_index = 0;

    optind = 1;
    opterr = 0;
    unsigned seen = 0;
    int explicit_command = CMD_NONE;

    while ((opt = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c':
                if (copy_cli_path(opts->config_file, sizeof(opts->config_file), optarg, "--config") != 0) return -1;
                seen |= OPTION_CONFIG;
                break;
            case 'p':
                if (copy_cli_path(opts->pid_file, sizeof(opts->pid_file), optarg, "--pid") != 0) return -1;
                seen |= OPTION_PID;
                break;
            case 'f':
                opts->run_mode = CLI_RUN_MODE_FOREGROUND;
                seen |= OPTION_FOREGROUND;
                break;
            case 'd':
                opts->run_mode = CLI_RUN_MODE_DAEMON;
                seen |= OPTION_DAEMON;
                break;
            case 'V':
                opts->verbosity = CLI_VERBOSITY_VERBOSE;
                seen |= OPTION_VERBOSE;
                break;
            case 'q':
                opts->verbosity = CLI_VERBOSITY_QUIET;
                seen |= OPTION_QUIET;
                break;
            case 'n':
                opts->no_color = true;
                seen |= OPTION_NO_COLOR;
                break;
            case 'h':
                explicit_command = CMD_HELP;
                seen |= OPTION_HELP;
                break;
            case 'v':
                explicit_command = CMD_VERSION;
                seen |= OPTION_VERSION;
                break;
            case ':':
                fprintf(stderr, "option '-%c' requires an argument\n", optopt);
                return -1;
            case '?':
                fprintf(stderr, "unknown option: %s\n", optopt ? argv[optind - 1] : "?");
                return -1;
            default:
                return -1;
        }
    }

    if (optind < argc) {
        if (argc - optind != 1 || command_from_string(argv[optind], &opts->command) != 0) {
            fprintf(stderr, "unknown or trailing command argument: %s\n", argv[optind]);
            return -1;
        }
    } else if (explicit_command != CMD_NONE) {
        opts->command = (atp_command_t)explicit_command;
    } else {
        opts->command = CMD_HELP;
    }

    if (explicit_command != CMD_NONE && opts->command != (atp_command_t)explicit_command) {
        fprintf(stderr, "help/version cannot be combined with a command\n");
        return -1;
    }
    return validate_options(opts->command, seen);
}

const char* command_to_string(atp_command_t cmd) {
    switch (cmd) {
        case CMD_START: return "start";
        case CMD_STOP: return "stop";
        case CMD_RESTART: return "restart";
        case CMD_STATUS: return "status";
        case CMD_RELOAD: return "reload";
        case CMD_CHECK: return "check";
        case CMD_VERSION: return "version";
        case CMD_HELP: return "help";
        default: return "unknown";
    }
}
