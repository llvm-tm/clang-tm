/*
 * tsx_spurious.c — Ground-truth TSX (RTM) spurious-abort prevalence probe.
 *
 * Purpose
 * -------
 * Real TSX transactions can abort even with *zero* data contention: a timer
 * interrupt, a cache eviction, a TLB miss, an SMI, or a sibling hyper-thread
 * touching the same L1 set can all trigger an abort.  These "spurious" aborts
 * are inherently hard to model (they depend on the OS, the clock source, and
 * microarchitectural effects), but a simulator can still *calibrate* how
 * prevalent they are by matching the single-thread abort rate measured here.
 *
 * The probe runs a fully isolated transaction (one line, no contention, no
 * writes) many times on a single pinned thread and reports the abort rate.
 * A well-behaved quiet machine should show < 0.01% (our intel14v2 runs show
 * ~0.001%).
 *
 * Build / run:
 *     gcc -O2 -mrtm -pthread -o tsx_spurious tsx_spurious.c
 *     ./tsx_spurious 1000000
 *     ./tsx_spurious 1000000 4   # with 4 background cache-thrash threads
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <immintrin.h>
#include <sched.h>

#define CACHELINE 64
#define THRASH_BYTES (8 * 1024 * 1024)   /* 8 MiB, exceeds L2, some L3 noise */

static volatile uint64_t trash[THRASH_BYTES / 8];

static void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}

/* background thread: continuously write a moving line, forcing cache-coherence
 * traffic that may evict the measured transaction's lines */
static void *thrasher(void *arg) {
    int id = (int)(intptr_t)arg;
    pin(id % 4);
    long stride = (id + 1) * 1024; /* lines apart */
    while (1) {
        for (long i = 0; i < THRASH_BYTES / 8; i += stride)
            trash[i] = (uint64_t)i;
    }
    return NULL;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    int thrash_threads = argc > 2 ? atoi(argv[2]) : 0;

    pthread_t th[8];
    for (int i = 0; i < thrash_threads && i < 8; i++)
        pthread_create(&th[i], NULL, thrasher, (void *)(intptr_t)i);

    pin(0);

    volatile uint64_t line[CACHELINE / 8] = {0};
    long commits = 0, aborts = 0;

    for (long i = 0; i < n; i++) {
        unsigned status = _xbegin();
        if (status == _XBEGIN_STARTED) {
            /* tiny transaction: read the line, no writes */
            uint64_t x = line[0];
            (void)x;
            _xend();
            commits++;
        } else {
            aborts++;
        }
    }

    printf("iterations=%ld background_thrash_threads=%d\n", n, thrash_threads);
    printf("commits=%ld aborts=%ld\n", commits, aborts);
    if (commits + aborts > 0)
        printf("spurious abort rate = %.6f%%\n",
               100.0 * aborts / (commits + aborts));
    return 0;
}
