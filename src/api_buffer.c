#include "api_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/*
 * ============================================================================
 * API Buffer Lifecycle
 * ============================================================================
 *
 * init()
 *   |
 *   v
 * append() <----+
 *   |           |
 *   v           |
 * reset() ------+
 *   |
 *   v
 * free()
 *
 * ============================================================================
 * Rules:
 *
 * 1. init() must be called before any other operation.
 * 2. init() must not be called twice without free() in between.
 * 3. append() can be called multiple times after init().
 * 4. reset() clears data, keeps capacity, can be called multiple times.
 * 5. reset() does not free memory, only resets len to 0.
 * 6. free() releases all memory, must be called at end.
 * 7. After free(), buffer can be init() again.
 * 8. All APIs accept NULL safely.
 *
 * ============================================================================
 * Debug Modes:
 *
 * API_BUFFER_DEBUG         - Enable invariant checking
 * API_BUFFER_DEBUG_ASSERT  - Enable assert() on invariant violation
 * API_BUFFER_SECURE_ZERO   - Zero sensitive data on reset/free
 *
 * ============================================================================
 */

#ifndef API_BUFFER_MIN_SIZE
#define API_BUFFER_MIN_SIZE 64
#endif

#ifndef API_BUFFER_MAX_SIZE
#define API_BUFFER_MAX_SIZE (16 * 1024 * 1024)
#endif

#ifdef API_BUFFER_DEBUG
#include <stdio.h>

static int api_buffer_validate(const api_buffer_t *buf) {
    if (!buf) {
        fprintf(stderr, "api_buffer: NULL buffer pointer\n");
        return -1;
    }

    if (buf->data == NULL) {
        if (buf->cap != 0 || buf->len != 0) {
            fprintf(stderr, "api_buffer: inconsistent NULL state (cap=%zu, len=%zu)\n",
                    buf->cap, buf->len);
            return -1;
        }
    } else {
        if (buf->cap == 0) {
            fprintf(stderr, "api_buffer: data non-NULL but cap=0\n");
            return -1;
        }
        if (buf->len > buf->cap) {
            fprintf(stderr, "api_buffer: len=%zu > cap=%zu\n", buf->len, buf->cap);
            return -1;
        }
        if (buf->cap > API_BUFFER_MAX_SIZE) {
            fprintf(stderr, "api_buffer: cap=%zu > API_BUFFER_MAX_SIZE=%zu\n",
                    buf->cap, (size_t)API_BUFFER_MAX_SIZE);
            return -1;
        }
    }

    return 0;
}

#define API_BUFFER_VALIDATE(buf) api_buffer_validate((buf))

#ifdef API_BUFFER_DEBUG_ASSERT
#include <assert.h>
#define API_BUFFER_ASSERT(buf) assert(api_buffer_validate((buf)) == 0)
#else
#define API_BUFFER_ASSERT(buf) ((void)0)
#endif

#else
#define API_BUFFER_VALIDATE(buf) (0)
#define API_BUFFER_ASSERT(buf) ((void)0)
#endif

#ifdef API_BUFFER_SECURE_ZERO
static void secure_memzero(void *ptr, size_t len) {
    if (!ptr || len == 0) return;

    volatile unsigned char *p = (volatile unsigned char *)ptr;
    volatile unsigned char *end = p + len;

    while (p < end) {
        *p++ = 0;
    }
}
#else
static void secure_memzero(void *ptr, size_t len) {
    (void)ptr;
    (void)len;
}
#endif

int api_buffer_init(api_buffer_t *buf, size_t initial_cap) {
    if (!buf) return -1;

    /* Prevent double init */
    if (buf->data != NULL) {
        return -1;
    }

    if (initial_cap == 0) {
        initial_cap = API_BUFFER_MIN_SIZE;
    }

    if (initial_cap > API_BUFFER_MAX_SIZE) {
        return -1;
    }

    buf->data = malloc(initial_cap);
    if (!buf->data) {
        buf->data = NULL;
        buf->len = 0;
        buf->cap = 0;
        return -1;
    }

    buf->len = 0;
    buf->cap = initial_cap;

    API_BUFFER_ASSERT(buf);
    return 0;
}

int api_buffer_append(api_buffer_t *buf, const char *src, size_t src_len) {
    if (!buf) return -1;

    /* Pre-validate */
    if (API_BUFFER_VALIDATE(buf) != 0) {
        return -1;
    }

    /* Empty write is always valid */
    if (src_len == 0) {
        return 0;
    }

    /* NULL source with non-zero length is invalid */
    if (src == NULL) {
        return -1;
    }

    /* Check for integer overflow */
    if (src_len > SIZE_MAX - buf->len) {
        return -1;
    }

    size_t needed = buf->len + src_len;

    /* Check against maximum capacity */
    if (needed > API_BUFFER_MAX_SIZE) {
        return -1;
    }

    /* Grow if needed */
    if (needed > buf->cap) {
        size_t new_cap = buf->cap == 0 ? API_BUFFER_MIN_SIZE : buf->cap;

        while (new_cap < needed) {
            if (new_cap > API_BUFFER_MAX_SIZE / 2) {
                new_cap = API_BUFFER_MAX_SIZE;
                break;
            }
            new_cap *= 2;
        }

        /* Verify growth result */
        if (new_cap < needed || new_cap > API_BUFFER_MAX_SIZE) {
            return -1;
        }

        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            return -1;
        }

        buf->data = new_data;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, src, src_len);
    buf->len += src_len;

    /* Post-validate */
    API_BUFFER_ASSERT(buf);
    return 0;
}

void api_buffer_free(api_buffer_t *buf) {
    if (!buf) return;

    if (API_BUFFER_VALIDATE(buf) != 0) {
        return;
    }

    if (buf->data) {
#ifdef API_BUFFER_SECURE_ZERO
        secure_memzero(buf->data, buf->cap);
#endif
        free(buf->data);
        buf->data = NULL;
    }

    buf->len = 0;
    buf->cap = 0;
}

void api_buffer_reset(api_buffer_t *buf) {
    if (!buf) return;

    if (API_BUFFER_VALIDATE(buf) != 0) {
        return;
    }

    if (buf->data && buf->len > 0) {
#ifdef API_BUFFER_SECURE_ZERO
        secure_memzero(buf->data, buf->len);
#endif
    }

    buf->len = 0;

    API_BUFFER_ASSERT(buf);
}
