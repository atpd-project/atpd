#ifndef ATP_CLI_H
#define ATP_CLI_H

#include <getopt.h>
#include <stdbool.h>
#include <limits.h>

typedef enum {
    CMD_NONE = 0,
    CMD_START,
    CMD_STOP,
    CMD_RESTART,
    CMD_STATUS,
    CMD_RELOAD,
    CMD_CHECK,
    CMD_VERSION,
    CMD_HELP
} atp_command_t;

typedef enum {
    CLI_RUN_MODE_DEFAULT = 0,
    CLI_RUN_MODE_FOREGROUND,
    CLI_RUN_MODE_DAEMON
} cli_run_mode_t;

typedef enum {
    CLI_VERBOSITY_DEFAULT = 0,
    CLI_VERBOSITY_VERBOSE,
    CLI_VERBOSITY_QUIET
} cli_verbosity_t;

typedef struct {
    atp_command_t command;
    char config_file[PATH_MAX];
    char pid_file[PATH_MAX];
    cli_run_mode_t run_mode;
    cli_verbosity_t verbosity;
    bool no_color;
} atp_options_t;

void print_usage(const char *progname);
void print_version(void);
void print_help(const char *progname);
int parse_arguments(int argc, char *argv[], atp_options_t *opts);
const char* command_to_string(atp_command_t cmd);

#endif
