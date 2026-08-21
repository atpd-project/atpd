#ifndef ATPD_UDS_H
#define ATPD_UDS_H

#include "reactor.h"
#include <stdio.h>

#define ATPD_UDS_PATH "/data/local/tmp/atpd.sock"

int uds_init(reactor_t *r, const char *path);
void uds_cleanup(void);
int uds_get_fd(void);
int uds_stop_requested(void);
int uds_reload_requested(void);
void uds_clear_requests(void);
int uds_client_request(const char *path, const char *command, FILE *output);
int uds_client_status(const char *path, FILE *output);
int uds_client_singbox_status(const char *path, FILE *output);

#endif
