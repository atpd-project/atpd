#ifndef ATP_API_BUFFER_H
#define ATP_API_BUFFER_H

#include <stddef.h>

#define API_BUFFER_MAX_SIZE (1024 * 1024)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} api_buffer_t;

int api_buffer_init(api_buffer_t *buf, size_t initial_cap);
int api_buffer_append(api_buffer_t *buf, const char *src, size_t src_len);
void api_buffer_free(api_buffer_t *buf);
void api_buffer_reset(api_buffer_t *buf);

#endif
