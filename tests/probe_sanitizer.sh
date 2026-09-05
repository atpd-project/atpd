#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-}"

case "$MODE" in
  asan)
    cat << 'EOF' > /tmp/probe_asan.c
#include <stdlib.h>
int main(void) {
    volatile char *p = (char *)malloc(16);
    char x = p[24];
    free((void *)p);
    return (int)x;
}
EOF
    zig cc -fsanitize=address -o /tmp/probe_asan /tmp/probe_asan.c -lpthread -l:libasan.so.8
    out="$(/tmp/probe_asan 2>&1 || true)"
    if echo "$out" | grep -q "AddressSanitizer: heap-buffer-overflow"; then
      echo "PROBE_PASS: ASan runtime actively intercepting violations"
      exit 0
    else
      echo "PROBE_FAIL: ASan runtime did not intercept violation" >&2
      echo "$out" >&2
      exit 1
    fi
    ;;

  tsan)
    cat << 'EOF' > /tmp/probe_tsan.c
#include <pthread.h>
static volatile int val;
static pthread_barrier_t barrier;
static void *worker(void *arg) {
    (void)arg;
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 100000; i++) val++;
    return NULL;
}
int main(void) {
    pthread_t t;
    pthread_barrier_init(&barrier, NULL, 2);
    pthread_create(&t, NULL, worker, NULL);
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 100000; i++) val++;
    pthread_join(t, NULL);
    pthread_barrier_destroy(&barrier);
    return 0;
}
EOF
    zig cc -fsanitize=thread -o /tmp/probe_tsan /tmp/probe_tsan.c -lpthread
    rc=0
    out="$(/tmp/probe_tsan 2>&1)" || rc=$?
    if echo "$out" | grep -q "ThreadSanitizer: data race"; then
      echo "PROBE_PASS: TSan runtime actively intercepting data races"
      exit 0
    else
      echo "PROBE_FAIL: TSan runtime did not intercept data race (exit code: $rc)" >&2
      echo "$out" >&2
      exit 1
    fi
    ;;

  *)
    echo "Usage: $0 {asan|tsan}" >&2
    exit 2
    ;;
esac
