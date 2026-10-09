/*
 * tsx_isolate.c — which TSXSGL component loses updates on real HW?
 * Modes:
 *   tsx : _xbegin/_xend loop, infinite retries, no SGL fallback
 *   sgl : pthread_mutex critical section only (no RTM)
 *   both: TSX + sgl_owner line + mutex fallback (mimics TSXSGL_runtime)
 * Two threads, one shared uint64 counter, N +1 RMWs each (plain volatile
 * read/write inside the protection domain). Final == 2N iff no lost update.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>

static volatile uint64_t ctr __attribute__((aligned(64))) = 0;
static volatile uint64_t sgl_owner __attribute__((aligned(64))) = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int mode_tsx, mode_sgl, mode_both;
static int N = 100000;

static void *worker(void *arg) {
    long id = (long)arg;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(id == 0 ? 1 : 13, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
    for (int i = 0; i < N; i++) {
        if (mode_tsx) {
            for (;;) {
                unsigned st = _xbegin();
                if (st == _XBEGIN_STARTED) {
                    ctr = ctr + 1;
                    _xend();
                    break;
                }
            }
        } else if (mode_sgl) {
            pthread_mutex_lock(&mtx);
            ctr = ctr + 1;
            pthread_mutex_unlock(&mtx);
        } else { /* both: TSX first, mutex fallback after 5 fails */
            int in_tx = 0;
            for (int a = 0; a < 5 && !in_tx; a++) {
                unsigned st = _xbegin();
                if (st == _XBEGIN_STARTED) {
                    if (sgl_owner != 0) { _xabort(0xFF); continue; }
                    ctr = ctr + 1;
                    if (sgl_owner != 0) _xabort(0x01);
                    _xend();
                    in_tx = 1;
                }
            }
            if (!in_tx) {
                pthread_mutex_lock(&mtx);
                sgl_owner = 1;
                ctr = ctr + 1;
                sgl_owner = 0;
                pthread_mutex_unlock(&mtx);
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <tsx|sgl|both> [N]\n", argv[0]); return 2; }
    if (!strcmp(argv[1], "tsx")) mode_tsx = 1;
    else if (!strcmp(argv[1], "sgl")) mode_sgl = 1;
    else mode_both = 1;
    if (argc > 2) N = atoi(argv[2]);
    pthread_t t[2];
    for (long i = 0; i < 2; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    printf("mode=%s final=%llu expected=%d -> %s\n", argv[1],
           (unsigned long long)ctr, 2 * N,
           ctr == (uint64_t)(2 * N) ? "PASS" : "LOST-UPDATE");
    return ctr == (uint64_t)(2 * N) ? 0 : 1;
}
