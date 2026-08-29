#include "atp_config.h"

#include <assert.h>

int main(void) {
    atp_config_t source = {0};
    source.service.restart_delay_sec = 2;
    source.api.port = 9080;

    atp_config_t copy = source;
    assert(copy.service.restart_delay_sec == source.service.restart_delay_sec);
    assert(copy.api.port == source.api.port);
    return 0;
}
