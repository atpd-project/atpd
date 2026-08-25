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
} singbox_api_ctx_t;

/* Snapshot returned by StartedService.SubscribeStatus. Values use the same
 * protobuf semantics as the official sing-box dashboard. */
typedef struct {
    uint64_t memory;
    int32_t goroutines;
    int32_t connections_in;
    int32_t connections_out;
    bool traffic_available;
    int64_t uplink;
    int64_t downlink;
    int64_t uplink_total;
    int64_t downlink_total;
} singbox_status_t;

#define SINGBOX_MAX_CLASH_MODES 32
#define SINGBOX_CLASH_MODE_SIZE 64

typedef struct {
    char modes[SINGBOX_MAX_CLASH_MODES][SINGBOX_CLASH_MODE_SIZE];
    size_t mode_count;
    char current_mode[SINGBOX_CLASH_MODE_SIZE];
} singbox_clash_mode_status_t;

/* Lifecycle */
int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg);
void singbox_api_cleanup(singbox_api_ctx_t *ctx);

/* Health Probe & Telemetry */
int singbox_api_health_check(singbox_api_ctx_t *ctx);
int singbox_api_get_status(singbox_api_ctx_t *ctx, singbox_status_t *status_out);
int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version_buf, size_t buf_size);
int singbox_api_get_goroutines(singbox_api_ctx_t *ctx, int *goroutines_out);
int singbox_api_get_clash_mode_status(singbox_api_ctx_t *ctx,
                                      singbox_clash_mode_status_t *status_out);
int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode_buf, size_t buf_size);
int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode);
int singbox_api_reload(singbox_api_ctx_t *ctx);

/* Direct CLI Bridge for sing-box api */
int singbox_api_exec_cli(const singbox_api_ctx_t *ctx, const char *subcmd,
                         char *output, size_t out_size, int timeout_sec);

#endif
