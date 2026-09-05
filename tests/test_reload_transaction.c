#include "config.h"
#include "reactor.h"
#include "service.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int fail_timer_add;

reactor_timer_t *__real_reactor_add_timer(reactor_t *r, uint64_t timeout_ms,
                                          uint64_t interval_ms,
                                          reactor_timer_cb cb, void *userdata);

reactor_timer_t *__wrap_reactor_add_timer(reactor_t *r, uint64_t timeout_ms,
                                          uint64_t interval_ms,
                                          reactor_timer_cb cb, void *userdata) {
    if (fail_timer_add) return NULL;
    return __real_reactor_add_timer(r, timeout_ms, interval_ms, cb, userdata);
}

static void noop_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;
}

static int expect_restart_required(const atp_config_t *current,
                                   const atp_config_t *candidate,
                                   const char *field) {
    char changed[512];
    config_reload_changes_t changes =
        config_classify_reload(current, candidate, changed, sizeof(changed));
    if (!(changes & CONFIG_RELOAD_CHANGE_REQUIRES_RESTART)) {
        fprintf(stderr, "expected restart-required field %s\n", field);
        return -1;
    }
    if (!strstr(changed, field)) {
        fprintf(stderr, "restart-required field %s missing from: %s\n", field, changed);
        return -1;
    }
    return 0;
}

static int test_field_classification(void) {
    atp_config_t current;
    atp_config_t candidate;
    char changed[512];
    config_set_defaults(&current);

#define CHECK_STRING_FIELD(member, value, name) do { \
        candidate = current; \
        snprintf(candidate.member, sizeof(candidate.member), "%s", value); \
        CHECK(expect_restart_required(&current, &candidate, name) == 0); \
    } while (0)
#define CHECK_INT_FIELD(member, name) do { \
        candidate = current; \
        candidate.member++; \
        CHECK(expect_restart_required(&current, &candidate, name) == 0); \
    } while (0)

    CHECK_STRING_FIELD(core.data_dir, "/different/data", "DATA_DIR");
    CHECK_STRING_FIELD(core.run_dir, "/different/run", "RUN_DIR");
    CHECK_STRING_FIELD(core.pid_file, "/different/atpd.pid", "PID_FILE");
    CHECK_STRING_FIELD(core.log_file, "/different/atp.log", "LOG_FILE");
    CHECK_STRING_FIELD(core.core_user, "different-user", "CORE_USER_GROUP");
    CHECK_STRING_FIELD(core.core_group, "different-group", "CORE_USER_GROUP");
    CHECK_STRING_FIELD(service.args, "-c different.json", "SERVICE_ARGS");
    CHECK_STRING_FIELD(service.env, "KEY=value", "SERVICE_ENV");
    CHECK_STRING_FIELD(api.host, "127.0.0.2", "API_HOST");
    CHECK_INT_FIELD(api.port, "API_PORT");
    CHECK_STRING_FIELD(api.secret, "different-secret", "API_SECRET");
    candidate = current;
    candidate.interface.vpn_auto_mode = !candidate.interface.vpn_auto_mode;
    CHECK(expect_restart_required(&current, &candidate, "VPN_AUTO_MODE") == 0);
    CHECK_STRING_FIELD(interface.vpn_target_mode, "Different", "VPN_TARGET_MODE");
    CHECK_STRING_FIELD(interface.vpn_fallback_mode, "Different", "VPN_FALLBACK_MODE");
    CHECK_INT_FIELD(service.circuit_threshold, "SERVICE_CIRCUIT_THRESHOLD");
    CHECK_INT_FIELD(service.circuit_cooldown_sec, "SERVICE_CIRCUIT_COOLDOWN");

    candidate = current;
    candidate.core.ui_emoji_enabled = !candidate.core.ui_emoji_enabled;
    candidate.service.start_timeout_sec++;
    candidate.service.stop_timeout_sec++;
    candidate.service.max_failures++;
    candidate.service.health_check_interval_ms++;
    config_reload_changes_t changes =
        config_classify_reload(&current, &candidate, changed, sizeof(changed));
    CHECK(changes == CONFIG_RELOAD_CHANGE_HOT);
    CHECK(changed[0] == '\0');

    candidate = current;
    candidate.core.log_timestamp = !candidate.core.log_timestamp;
    candidate.service.restart_delay_sec++;
    candidate.service.grace_period_sec++;
    changes = config_classify_reload(&current, &candidate, changed, sizeof(changed));
    CHECK(changes == CONFIG_RELOAD_CHANGE_STATIC);

#undef CHECK_STRING_FIELD
#undef CHECK_INT_FIELD
    return 0;
}

static int test_service_apply_is_transactional(void) {
    reactor_t *reactor = reactor_create();
    CHECK(reactor != NULL);

    service_ctx_t service;
    memset(&service, 0, sizeof(service));
    service.reactor = reactor;
    service.state = SERVICE_RUNNING;
    service.api_port = 9080;
    service.max_failures = 5;
    service.start_timeout_sec = 30;
    service.stop_timeout_sec = 10;
    service.grace_period_sec = 3;
    service.health_check_interval_ms = 5000;
    snprintf(service.user, sizeof(service.user), "root");
    snprintf(service.group, sizeof(service.group), "root");
    snprintf(service.service_args, sizeof(service.service_args), "old-args");
    snprintf(service.service_env, sizeof(service.service_env), "OLD=1");
    service.health_timer = __real_reactor_add_timer(reactor, 5000, 5000,
                                                     noop_timer_cb, &service);
    CHECK(service.health_timer != NULL);
    reactor_timer_t *old_timer = service.health_timer;

    atp_config_t candidate;
    config_set_defaults(&candidate);
    candidate.service.max_failures = 6;
    candidate.service.start_timeout_sec = 31;
    candidate.service.stop_timeout_sec = 11;
    candidate.service.health_check_interval_ms = 250;
    candidate.api.port = 9999;
    snprintf(candidate.core.core_user, sizeof(candidate.core.core_user), "nobody");
    snprintf(candidate.core.core_group, sizeof(candidate.core.core_group), "nogroup");
    snprintf(candidate.service.args, sizeof(candidate.service.args), "new-args");
    snprintf(candidate.service.env, sizeof(candidate.service.env), "NEW=1");

    fail_timer_add = 1;
    CHECK(service_apply_config(&service, &candidate) != 0);
    CHECK(service.health_timer == old_timer);
    CHECK(service.health_check_interval_ms == 5000);
    CHECK(service.max_failures == 5);
    CHECK(service.start_timeout_sec == 30);
    CHECK(service.stop_timeout_sec == 10);

    fail_timer_add = 0;
    CHECK(service_apply_config(&service, &candidate) == 0);
    CHECK(service.health_timer != NULL && service.health_timer != old_timer);
    CHECK(service.health_check_interval_ms == 250);
    CHECK(service.max_failures == 6);
    CHECK(service.start_timeout_sec == 31);
    CHECK(service.stop_timeout_sec == 11);
    CHECK(service.api_port == 9080);
    CHECK(strcmp(service.user, "root") == 0);
    CHECK(strcmp(service.group, "root") == 0);
    CHECK(strcmp(service.service_args, "old-args") == 0);
    CHECK(strcmp(service.service_env, "OLD=1") == 0);

    CHECK(reactor_cancel_timer(reactor, service.health_timer) == 0);
    service.health_timer = NULL;
    reactor_destroy(reactor);
    return 0;
}

int main(void) {
    CHECK(test_field_classification() == 0);
    CHECK(test_service_apply_is_transactional() == 0);
    puts("reload transaction unit tests passed");
    return 0;
}
