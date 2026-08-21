#ifndef ATP_CLI_H
#define ATP_CLI_H

#include <getopt.h>
#include <stdbool.h>
#include <limits.h>
#include "logger.h"

typedef enum {
    CMD_NONE = 0,
    CMD_START,
    CMD_STOP,
    CMD_RESTART,
    CMD_STATUS,
    CMD_UPDATE_GEOIP,
    CMD_RELOAD,
    CMD_CHECK,
    CMD_VERSION,
    CMD_HELP
} atp_command_t;

typedef struct {
    atp_command_t command;
    char config_file[PATH_MAX];
    char pid_file[PATH_MAX];
    int foreground;
    int daemon;
    int verbose;
    int quiet;
    int force;
    int no_color;
    int test_config;
    log_level_t log_level;
} atp_options_t;

void print_usage(const char *progname);
void print_version(void);
void print_help(const char *progname);
int parse_arguments(int argc, char *argv[], atp_options_t *opts);
const char* command_to_string(atp_command_t cmd);

#endif
