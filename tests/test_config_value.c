#include "atp_config.h"
#include <stdlib.h>

int main(void) {
    atp_config_t source = {0};
    source.service.restart_delay_sec = 2;
    source.api.port = 9080;

    atp_config_t copy = source;
    if (copy.service.restart_delay_sec != source.service.restart_delay_sec) abort();
    if (copy.api.port != source.api.port) abort();
    return 0;
}
