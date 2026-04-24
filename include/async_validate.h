#ifndef ATP_ASYNC_VALIDATE_H
#define ATP_ASYNC_VALIDATE_H

#include "reactor.h"
#include <sys/types.h>

typedef void (*validate_callback_t)(int result, const char *output, void *userdata);

typedef struct async_validate_ctx {
    reactor_t *reactor;
    pid_t child_pid;
    int pipe_fd;
    int timer_fd;
    char output[4096];
    size_t output_len;
    validate_callback_t callback;
    void *userdata;
    int completed;
} async_validate_ctx_t;

int async_validate_config(async_validate_ctx_t *ctx, reactor_t *r,
                          const char *bin_path, const char *work_dir,
                          validate_callback_t callback, void *userdata);

void async_validate_cleanup(async_validate_ctx_t *ctx);

#endif
