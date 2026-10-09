/*
 * tsx_conflict_matrix_gem5.c — gem5 SE variant of tsx_conflict_matrix.c.
 *
 * Same schedule (xbegin -> single access -> xend, two threads on one shared
 * line, one start barrier, no per-iteration handshake) so the abort pattern
 * observed under gem5's Ruby MESI_Three_Level_HTM can be compared against
 * the real-machine oracle in tsx_conflict_matrix.c / ground_truth_*.txt.
 *
 * Differences from the real probe (forced by gem5 SE):
 *   - iteration count is argv[2] (gem5 timing CPU is ~1000x slower than HW;
 *     20k iterations per thread keeps one mode to a few minutes of wall time)
 *   - pinning targets 1/2 instead of 1/13 (board has --threads+1 = 3 cores);
 *     if sched_setaffinity fails the run proceeds unpinned (SE clone places
 *     threads on distinct cores anyway)
 *
 * Build (x86-64 host): gcc -O2 -mrtm -pthread -static -o tsx_conflict_matrix_gem5 tsx_conflict_matrix_gem5.c
 * Run:  gem5.opt gem5_sim/configs/x86-se-bank.py --binary benchmarks/tsx/bin/tsx_conflict_matrix_gem5 \
 *         --threads 2 --raw-args "RW 20000"
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

#define CACHELINE 64

static volatile uint64_t line __attribute__((aligned(CACHELINE))) = 0;
static pthread_barrier_t start_barrier;
static _Atomic long c1 = 0, a1 = 0, c2 = 0, a2 = 0;
static _Atomic long c1_conf = 0, c2_conf = 0;
static const char *mode;
static int iters = 20000;

static void pin_self(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s) != 0)
        fprintf(stderr, "warn: pin to cpu %d failed (unpinned run)\n", cpu);
}

static void *t1_fn(void *arg) {
    (void)arg;
    pin_self(1);
    int w = mode[0] == 'W';
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < iters; i++) {
        unsigned s = _xbegin();
        if (s != _XBEGIN_STARTED) {
            atomic_fetch_add(&a1, 1);
            if (s & _XABORT_CONFLICT) atomic_fetch_add(&c1_conf, 1);
            continue;
        }
        if (w) line = 1;
        else { volatile uint64_t v = line; (void)v; }
        _xend();
        atomic_fetch_add(&c1, 1);
    }
    return NULL;
}

static void *t2_fn(void *arg) {
    (void)arg;
    pin_self(2);
    int w = mode[1] == 'W';
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < iters; i++) {
        unsigned s = _xbegin();
        if (s != _XBEGIN_STARTED) {
            atomic_fetch_add(&a2, 1);
            if (s & _XABORT_CONFLICT) atomic_fetch_add(&c2_conf, 1);
            continue;
        }
        if (w) line = 2;
        else { volatile uint64_t v = line; (void)v; }
        _xend();
        atomic_fetch_add(&c2, 1);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <RR|RW|WR|WW> [iters]\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    if (argc > 2) iters = atoi(argv[2]);
    if (strcmp(mode, "RR") && strcmp(mode, "RW") && strcmp(mode, "WR") && strcmp(mode, "WW")) {
        fprintf(stderr, "unknown mode '%s' (want RR|RW|WR|WW)\n", mode);
        return 2;
    }
    pthread_barrier_init(&start_barrier, NULL, 2);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, t1_fn, NULL);
    pthread_create(&t2, NULL, t2_fn, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    long C1 = atomic_load(&c1), A1 = atomic_load(&a1);
    long C2 = atomic_load(&c2), A2 = atomic_load(&a2);
    printf("mode=%s pinned 1/2 (gem5 SE) free-running %d iters (short TX: 1 access + xend)\n", mode, iters);
    printf("  T1 %s: commits=%ld aborts=%ld (%.1f%% abort, %ld conflict)\n",
           mode[0] == 'W' ? "W" : "R", C1, A1, 100.0 * A1 / (C1 + A1), atomic_load(&c1_conf));
    printf("  T2 %s: commits=%ld aborts=%ld (%.1f%% abort, %ld conflict)\n",
           mode[1] == 'W' ? "W" : "R", C2, A2, 100.0 * A2 / (C2 + A2), atomic_load(&c2_conf));
    return 0;
}
