#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/service.c"

static void init_ctx(service_ctx_t *ctx, const char *args) {
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->bin_path, sizeof(ctx->bin_path), "/bin/sing-box");
    snprintf(ctx->work_dir, sizeof(ctx->work_dir), "/work");
    snprintf(ctx->conf_path, sizeof(ctx->conf_path), "/default.json");
    snprintf(ctx->service_args, sizeof(ctx->service_args), "%s", args);
}

static void expect_args(service_ctx_t *ctx, const char *const *expected) {
    char **actual = build_service_args(ctx);
    if (!actual) abort();
    for (size_t i = 0; expected[i] || actual[i]; i++) {
        if (!expected[i] || !actual[i] || strcmp(expected[i], actual[i]) != 0) abort();
    }
    free_service_args(actual);
}

int main(void) {
    service_ctx_t ctx;

    init_ctx(&ctx, "-c /one.json -c /two.json");
    const char *double_c[] = {
        "/bin/sing-box", "run", "-D", "/work",
        "-c", "/one.json", "-c", "/two.json", NULL
    };
    expect_args(&ctx, double_c);

    init_ctx(&ctx, "-C /configs");
    const char *capital_c[] = {
        "/bin/sing-box", "run", "-D", "/work", "-C", "/configs", NULL
    };
    expect_args(&ctx, capital_c);

    init_ctx(&ctx, "-cpu 2");
    const char *substring[] = {
        "/bin/sing-box", "run", "-D", "/work",
        "-c", "/default.json", "-cpu", "2", NULL
    };
    expect_args(&ctx, substring);

    init_ctx(&ctx, "--name=\"two words\"");
    if (build_service_args(&ctx) != NULL) abort();
    init_ctx(&ctx, "--name='unterminated");
    if (build_service_args(&ctx) != NULL) abort();
    init_ctx(&ctx, "two\\ words");
    if (build_service_args(&ctx) != NULL) abort();

    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.service_env, sizeof(ctx.service_env),
             "GOOD=value EMPTY= WITH_EQUALS=a=b _OK=$VAR");
    if (set_service_environment(&ctx) != 0 ||
        strcmp(getenv("GOOD"), "value") != 0 ||
        strcmp(getenv("EMPTY"), "") != 0 ||
        strcmp(getenv("WITH_EQUALS"), "a=b") != 0 ||
        strcmp(getenv("_OK"), "$VAR") != 0) abort();

    const char *invalid_env[] = {
        "NO_EQUALS", "=empty", "9KEY=value", "BAD-KEY=value",
        "GOOD=value trailing", "QUOTED=\"two words\"", NULL
    };
    for (size_t i = 0; invalid_env[i]; i++) {
        snprintf(ctx.service_env, sizeof(ctx.service_env), "%s", invalid_env[i]);
        if (set_service_environment(&ctx) == 0) abort();
    }

    puts("Service parsing tests passed");
    return 0;
}
