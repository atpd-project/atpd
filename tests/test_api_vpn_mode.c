#include "api.h"
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static singbox_clash_mode_status_t mock_status;
static int set_calls;
static char last_set_mode[SINGBOX_CLASH_MODE_SIZE];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
}

int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg) {
    (void)ctx;
    (void)cfg;
    return 0;
}

void singbox_api_cleanup(singbox_api_ctx_t *ctx) { (void)ctx; }
int reactor_add_fd(reactor_t *r, int fd, uint32_t events,
                   reactor_io_cb cb, void *userdata) {
    (void)r; (void)fd; (void)events; (void)cb; (void)userdata;
    return -1;
}
int reactor_remove_fd(reactor_t *r, int fd) { (void)r; (void)fd; return 0; }
uint64_t reactor_now_ms(void) { return 0; }
int singbox_api_health_check(singbox_api_ctx_t *ctx) { (void)ctx; return 0; }
int singbox_api_get_status(singbox_api_ctx_t *ctx, singbox_status_t *status) {
    (void)ctx;
    (void)status;
    return -1;
}
int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version, size_t size) {
    (void)ctx;
    (void)version;
    (void)size;
    return -1;
}
int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode, size_t size) {
    (void)ctx;
    snprintf(mode, size, "%s", mock_status.current_mode);
    return 0;
}
int singbox_api_get_clash_mode_status(singbox_api_ctx_t *ctx,
                                      singbox_clash_mode_status_t *status) {
    (void)ctx;
    *status = mock_status;
    return 0;
}
int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode) {
    (void)ctx;
    set_calls++;
    snprintf(last_set_mode, sizeof(last_set_mode), "%s", mode);
    snprintf(mock_status.current_mode, sizeof(mock_status.current_mode), "%s", mode);
    return 0;
}

static void set_current_mode(const char *mode) {
    snprintf(mock_status.current_mode, sizeof(mock_status.current_mode), "%s", mode);
}

int main(void) {
    api_ctx_t ctx = {0};
    atp_config_t config = {0};
    CHECK(api_init(&ctx, &config) == 0);
    config.interface.vpn_auto_mode = true;
    snprintf(config.interface.vpn_target_mode,
             sizeof(config.interface.vpn_target_mode), "Google VPN");
    snprintf(config.interface.vpn_fallback_mode,
             sizeof(config.interface.vpn_fallback_mode), "Rule");

    const char *modes[] = {"Rule", "Global", "Direct", "Google VPN"};
    mock_status.mode_count = sizeof(modes) / sizeof(modes[0]);
    for (size_t i = 0; i < mock_status.mode_count; ++i) {
        snprintf(mock_status.modes[i], sizeof(mock_status.modes[i]), "%s", modes[i]);
    }

    set_current_mode("Global");
    api_vpn_mode_callback(VPN_STATE_READY, "tun0", &ctx);
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    CHECK(strcmp(last_set_mode, "Google VPN") == 0);
    api_vpn_mode_callback(VPN_STATE_IDLE, "", &ctx);
    CHECK(strcmp(last_set_mode, "Global") == 0);
    CHECK(ctx.default_mode[0] == '\0');

    set_current_mode("Direct");
    api_vpn_mode_callback(VPN_STATE_READY, "wg0", &ctx);
    CHECK(strcmp(ctx.default_mode, "Direct") == 0);
    api_vpn_mode_callback(VPN_STATE_IDLE, "", &ctx);
    CHECK(strcmp(last_set_mode, "Direct") == 0);
    CHECK(ctx.default_mode[0] == '\0');

    int calls_before = set_calls;
    set_current_mode("Google VPN");
    api_vpn_mode_callback(VPN_STATE_READY, "ipsec0", &ctx);
    CHECK(strcmp(ctx.default_mode, "Google VPN") == 0);
    CHECK(set_calls == calls_before);
    api_vpn_mode_callback(VPN_STATE_IDLE, "", &ctx);
    CHECK(set_calls == calls_before);
    CHECK(ctx.default_mode[0] == '\0');

    set_current_mode("Global");
    api_vpn_mode_callback(VPN_STATE_IDLE, "", &ctx);
    CHECK(set_calls == calls_before);

    api_cleanup(&ctx);
    puts("VPN mode state tests passed");
    return 0;
}
