/*
 * tsx_conflict_matrix.c — Ground-truth TSX (RTM) conflict-semantics probe.
 *
 * Purpose
 * -------
 * Capture how *real* Intel TSX resolves contention between two concurrent
 * transactions on the same cache line, across all four access schedules:
 *
 *     RR  : T1 reads X,  T2 reads X   (no conflict expected -> both commit)
 *     RW  : T1 reads X,  T2 writes X  (write invalidates reader -> reader aborts)
 *     WR  : T1 writes X, T2 reads X   (symmetric: reader aborts)
 *     WW  : T1 writes X, T2 writes X  (short TX: rarely overlaps; longer TX: one aborts)
 *
 * For each schedule we record, per-thread, who COMMITS and who ABORTS.
 * This is the oracle the TSX simulator (runtime/tsx_sim) must reproduce.
 *
 * Mechanism
 * ---------
 * Two threads pinned to different physical cores (1 and 13 on a 14-core
 * Broadwell) free-run 200k iterations each: xbegin -> single access -> xend.
 * A single pthread_barrier synchronizes start; afterwards no per-iteration
 * handshake is used (a per-iteration barrier causes 100% aborts due to the
 * barrier's own futex/lock traffic; a handshake inside TX causes a false
 * RW conflict on the handshake flag itself).  Transactions are intentionally
 * kept short (one access + xend) so the abort rate reflects conflict
 * semantics, not timer-interrupt or capacity time-outs (a 2000-pause TX
 * aborts ~100% spuriously on this hardware).
 *
 * Build / run (on a TSX-capable machine, e.g. intel14v2):
 *     gcc -O2 -mrtm -pthread -o tsx_conflict_matrix tsx_conflict_matrix.c
 *     ./tsx_conflict_matrix RR
 *     ./tsx_conflict_matrix RW
 *     ./tsx_conflict_matrix WR
 *     ./tsx_conflict_matrix WW
 *
 * Ground-truth results on intel14v2 (E5-2660 v4, Broadwell-EP, pinned 1/13):
 *   RR  T1 R 199994/6   (0.0% abort)  T2 R 199998/2   (0.0% abort)  -- both commit
 *   RW  T1 R  63220/136780 (68.4%)    T2 W 199815/185  (0.1%)      -- reader aborts
 *   WR  T1 W 199719/281   (0.1%)      T2 R  67222/132778 (66.4%)   -- reader aborts
 *   WW  T1 W 199972/28   (0.0%)       T2 W 199971/29   (0.0%)      -- rarely overlaps (short TX)
 * Unpinned the same matrix shows ~50% higher abort rates due to migration
 * and sibling-hyperthread L1 sharing.
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

static void pin_self(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}

static void *t1_fn(void *arg) {
    (void)arg;
    pin_self(1);
    int w = mode[0] == 'W';
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < 200000; i++) {
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
    pin_self(13);
    int w = mode[1] == 'W';
    pthread_barrier_wait(&start_barrier);
    for (int i = 0; i < 200000; i++) {
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
        fprintf(stderr, "usage: %s <RR|RW|WR|WW>\n", argv[0]);
        return 2;
    }
    mode = argv[1];
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
    printf("mode=%s pinned 1/13 free-running 200k iters (short TX: 1 access + xend)\n", mode);
    printf("  T1 %s: commits=%ld aborts=%ld (%.1f%% abort, %ld conflict)\n",
           mode[0] == 'W' ? "W" : "R", C1, A1, 100.0 * A1 / (C1 + A1), atomic_load(&c1_conf));
    printf("  T2 %s: commits=%ld aborts=%ld (%.1f%% abort, %ld conflict)\n",
           mode[1] == 'W' ? "W" : "R", C2, A2, 100.0 * A2 / (C2 + A2), atomic_load(&c2_conf));
    return 0;
}
