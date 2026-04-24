#include "api_buffer.h"
#include <stdlib.h>
#include <string.h>

int api_buffer_init(api_buffer_t *buf, size_t initial_cap) {
    if (initial_cap > API_BUFFER_MAX_SIZE) initial_cap = API_BUFFER_MAX_SIZE;
    buf->data = malloc(initial_cap);
    if (!buf->data) return -1;
    buf->len = 0;
    buf->cap = initial_cap;
    return 0;
}

int api_buffer_append(api_buffer_t *buf, const char *src, size_t src_len) {
    if (buf->len + src_len > buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap > API_BUFFER_MAX_SIZE) new_cap = API_BUFFER_MAX_SIZE;
        if (buf->len + src_len > new_cap) return -1;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return -1;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, src, src_len);
    buf->len += src_len;
    return 0;
}

void api_buffer_free(api_buffer_t *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

void api_buffer_reset(api_buffer_t *buf) {
    buf->len = 0;
}
