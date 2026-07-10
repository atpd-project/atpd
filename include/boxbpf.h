#ifndef BOXBPF_H
#define BOXBPF_H

#include <stdbool.h>
#include <stddef.h>
#include "atp.h"

int boxbpf_probe(bool ipv6);
int boxbpf_apply(const char *config_path);
int boxbpf_update(const char *config_path);
int boxbpf_clear(void);
bool boxbpf_is_ready(void);
const char *boxbpf_pin_dir(void);
int boxbpf_init_from_config(atp_config_t *cfg);
int boxbpf_reload_from_config(atp_config_t *cfg);
int boxbpf_status(char *state, size_t size, atp_config_t *cfg);

#endif
