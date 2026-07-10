#ifndef ATP_CLEANUP_H
#define ATP_CLEANUP_H

#include "atp.h"

void atp_register_cleanup(atp_config_t *cfg);
void atp_cleanup_all(void);

#endif
