#ifndef ATPD_UDS_H
#define ATPD_UDS_H

#include "reactor.h"

#define ATPD_UDS_PATH "/data/local/tmp/atpd.sock"

int uds_init(reactor_t *r, const char *path);
void uds_cleanup(void);
int uds_get_fd(void);

#endif
