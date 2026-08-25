#ifndef ATP_SINGBOX_API_H
#define ATP_SINGBOX_API_H

#include "atp.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    char host[64];
    int port;
    char secret[128];
    int timeout_sec;
    int connected;
    time_t last_check;
    char debug_host[64];
    int debug_port;
} singbox_api_ctx_t;

/* Lifecycle */
int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg);
void singbox_api_cleanup(singbox_api_ctx_t *ctx);

/* Health Probe & Telemetry */
int singbox_api_health_check(singbox_api_ctx_t *ctx);
int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version_buf, size_t buf_size);
int singbox_api_get_goroutines(singbox_api_ctx_t *ctx, int *goroutines_out);
int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode_buf, size_t buf_size);
int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode);
int singbox_api_reload(singbox_api_ctx_t *ctx);

/* Direct CLI Bridge for sing-box api */
int singbox_api_exec_cli(const singbox_api_ctx_t *ctx, const char *subcmd,
                         char *output, size_t out_size, int timeout_sec);

#endif
