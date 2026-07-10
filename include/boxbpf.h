#ifndef ATP_BOXBPF_H
#define ATP_BOXBPF_H

#include <stdbool.h>
#include "atp.h"

int boxbpf_probe(bool ipv6);
int boxbpf_apply(const char *config_path);
int boxbpf_update(const char *config_path);
int boxbpf_clear(void);
bool boxbpf_is_ready(void);
const char *boxbpf_pin_dir(void);
int boxbpf_init_from_config(atp_config_t *cfg);

#endif
