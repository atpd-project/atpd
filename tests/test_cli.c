#include "cli.h"

#include <assert.h>
#include <stddef.h>

static void expect_custom_config_command(const char *command,
                                         atp_command_t expected) {
    char *argv[] = {"atpd", "-c", "/tmp/custom.conf", (char *)command, NULL};
    atp_options_t opts;

    assert(parse_arguments(4, argv, &opts) == 0);
    assert(opts.command == expected);
}

int main(void) {
    expect_custom_config_command("stop", CMD_STOP);
    expect_custom_config_command("status", CMD_STATUS);
    expect_custom_config_command("reload", CMD_RELOAD);

    char *trailing[] = {"atpd", "status", "extra", NULL};
    atp_options_t opts;
    assert(parse_arguments(3, trailing, &opts) != 0);

    char *invalid_mode[] = {"atpd", "--foreground", "status", NULL};
    assert(parse_arguments(3, invalid_mode, &opts) != 0);
    return 0;
}
