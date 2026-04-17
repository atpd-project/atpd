#include "cli.h"
#include "logger.h"
#include "atp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *copyright =
    ATP_NAME " v" ATP_VERSION "\n"
    "Copyright (C) 2024 ATP Project\n"
    "License: GPL v3\n";

static const struct option long_options[] = {
    {"start",         no_argument,       0, 's'},
    {"stop",          no_argument,       0, 't'},
    {"restart",       no_argument,       0, 'r'},
    {"status",        no_argument,       0, 'u'},
    {"update-geoip",  no_argument,       0, 'g'},
    {"reload",        no_argument,       0, 'l'},
    {"config-dir",    required_argument, 0, 'd'},
    {"dry-run",       no_argument,       0, 'n'},
    {"verbose",       no_argument,       0, 'v'},
    {"quiet",         no_argument,       0, 'q'},
    {"foreground",    no_argument,       0, 'f'},
    {"syslog",        no_argument,       0, 'y'},
    {"log-file",      required_argument, 0, 'o'},
    {"log-level",     required_argument, 0, 'L'},
    {"help",          no_argument,       0, 'h'},
    {"version",       no_argument,       0, 'V'},
    {0, 0, 0, 0}
};

static const char *short_options = "strudglnd:vqfyso:L:hV";

void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS] COMMAND\n\n", progname);
    printf("Commands:\n");
    printf("  start              Start ATP daemon\n");
    printf("  stop               Stop ATP daemon\n");
    printf("  restart            Restart ATP daemon\n");
    printf("  status             Show service status\n");
    printf("  update-geoip       Update China GeoIP database\n");
    printf("  reload             Reload configuration\n");
    printf("\nOptions:\n");
    printf("  -d, --config-dir DIR   Set configuration directory\n");
    printf("  -n, --dry-run          Simulate operations (no changes)\n");
    printf("  -v, --verbose          Enable verbose output\n");
    printf("  -q, --quiet            Suppress non-error output\n");
    printf("  -f, --foreground       Run in foreground (don't daemonize)\n");
    printf("  -y, --syslog           Log to syslog\n");
    printf("  -o, --log-file FILE    Write logs to FILE\n");
    printf("  -L, --log-level LVL    Set log level\n");
    printf("  -h, --help             Show this help message\n");
    printf("  -V, --version          Show version information\n");
}

void print_version(void) {
    printf("%s", copyright);
    printf("Build: %s %s\n", __DATE__, __TIME__);
}

void print_help(const char *progname) {
    print_usage(progname);
}

int parse_arguments(int argc, char *argv[], atp_options_t *opts) {
    memset(opts, 0, sizeof(atp_options_t));
    opts->command = CMD_NONE;
    opts->log_level = LOG_LEVEL_INFO;
    
    strcpy(opts->config_dir, ATP_DEFAULT_DIR);
    
    int opt;
    int option_index = 0;
    
    optind = 1;
    opterr = 1;
    
    while ((opt = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (opt) {
            case 's':
                opts->command = CMD_START;
                break;
            case 't':
                opts->command = CMD_STOP;
                break;
            case 'r':
                opts->command = CMD_RESTART;
                break;
            case 'u':
                opts->command = CMD_STATUS;
                break;
            case 'g':
                opts->command = CMD_UPDATE_GEOIP;
                break;
            case 'l':
                opts->command = CMD_RELOAD;
                break;
            case 'd':
                strncpy(opts->config_dir, optarg, sizeof(opts->config_dir) - 1);
                opts->config_dir[sizeof(opts->config_dir) - 1] = '\0';
                break;
            case 'n':
                opts->dry_run = 1;
                break;
            case 'v':
                opts->verbose = 1;
                opts->log_level = LOG_LEVEL_DEBUG;
                break;
            case 'q':
                opts->quiet = 1;
                opts->log_level = LOG_LEVEL_ERROR;
                break;
            case 'f':
                opts->foreground = 1;
                break;
            case 'y':
                opts->use_syslog = 1;
                break;
            case 'o':
                strncpy(opts->log_file, optarg, sizeof(opts->log_file) - 1);
                opts->log_file[sizeof(opts->log_file) - 1] = '\0';
                break;
            case 'L':
                if (strcmp(optarg, "debug") == 0) opts->log_level = LOG_LEVEL_DEBUG;
                else if (strcmp(optarg, "info") == 0) opts->log_level = LOG_LEVEL_INFO;
                else if (strcmp(optarg, "warn") == 0) opts->log_level = LOG_LEVEL_WARN;
                else if (strcmp(optarg, "error") == 0) opts->log_level = LOG_LEVEL_ERROR;
                else {
                    fprintf(stderr, "Error: invalid log level '%s'\n", optarg);
                    return -1;
                }
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
    
    // Verify that every long option argument is exactly matched (no prefix matching)
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '-' && arg[1] == '-') {
            // This is a long option
            const char *optname = arg + 2;
            int exact_match = 0;
            for (int j = 0; long_options[j].name != NULL; j++) {
                if (strcmp(optname, long_options[j].name) == 0) {
                    exact_match = 1;
                    break;
                }
            }
            if (!exact_match) {
                fprintf(stderr, "Error: unknown option '--%s'\n", optname);
                return -1;
            }
        }
    }
    
    // Check for command if not already set by long option
    if (opts->command == CMD_NONE && optind < argc) {
        const char *cmd = argv[optind];
        if (strcmp(cmd, "start") == 0) opts->command = CMD_START;
        else if (strcmp(cmd, "stop") == 0) opts->command = CMD_STOP;
        else if (strcmp(cmd, "restart") == 0) opts->command = CMD_RESTART;
        else if (strcmp(cmd, "status") == 0) opts->command = CMD_STATUS;
        else if (strcmp(cmd, "update-geoip") == 0) opts->command = CMD_UPDATE_GEOIP;
        else if (strcmp(cmd, "reload") == 0) opts->command = CMD_RELOAD;
        else if (strcmp(cmd, "help") == 0) opts->command = CMD_HELP;
        else if (strcmp(cmd, "version") == 0) opts->command = CMD_VERSION;
        else {
            fprintf(stderr, "Error: unknown command '%s'\n", cmd);
            return -1;
        }
    }
    
    return 0;
}

const char* command_to_string(atp_command_t cmd) {
    switch (cmd) {
        case CMD_START: return "start";
        case CMD_STOP: return "stop";
        case CMD_RESTART: return "restart";
        case CMD_STATUS: return "status";
        case CMD_UPDATE_GEOIP: return "update-geoip";
        case CMD_RELOAD: return "reload";
        case CMD_VERSION: return "version";
        case CMD_HELP: return "help";
        default: return "unknown";
    }
}
