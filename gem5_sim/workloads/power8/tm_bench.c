/**
 * Minimal POWER8 HTM benchmark — fuzz-counter style.
 * N threads increment M shared counters inside transactions.
 * Verifies the invariant: final sum == initial sum + total committed.
 *
 * Compile: powerpc64le-linux-gnu-gcc -static -mcpu=power8 -pthread -O2 \
 *            -o tm_bench tm_bench.c
 *
 * Run in gem5: ./build/ALL/gem5.opt configs/example/se.py \
 *            --cmd workloads/power8/tm_bench --options "2 100 8 42" \
 *            --cpu-type TimingSimpleCPU --caches --num-cpus=2
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_num_counters;
static atomic_uint_least64_t *g_counters;
static atomic_int g_htm_ok = 0;
static atomic_int g_htm_fail = 0;
static atomic_int g_sgl_fallback = 0;

/* POWER8 HTM helpers */
static inline int tm_begin(void) {
    int ok;
    __asm__ __volatile__(
        "tbegin. 0\n\t"
        "mfocrf %0, 2\n\t"
        : "=r"(ok)
        :
        : "cr0", "memory"
    );
    return (ok & (1 << 2)) != 0;  /* CR0[EQ] */
}

static inline void tm_end(void) {
    __asm__ __volatile__(
        "tend. 0\n\t"
        :
        :
        : "memory"
    );
}

static inline void tm_abort(void) {
    __asm__ __volatile__(
        "tabort. 0\n\t"
        :
        :
        : "memory"
    );
}

static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

/* Retry transaction with SGL fallback */
static void tx_begin(void) {
    for (int i = 0; i < 5; i++) {
        if (tm_begin()) {
            atomic_fetch_add(&g_htm_ok, 1);
            return;
        }
        /* Spin briefly before retry */
        for (volatile int s = 0; s < 100; s++) {
            asm volatile("nop");
        }
    }
    /* SGL fallback */
    pthread_mutex_lock(&global_lock);
    atomic_fetch_add(&g_sgl_fallback, 1);
}

static void tx_end(void) {
    /* If we own the global lock, release it */
    if (pthread_mutex_trylock(&global_lock) == 0) {
        pthread_mutex_unlock(&global_lock);
        return;
    }
    tm_end();
}

static void tx_abort(void) {
    tm_abort();
}

/* Thread worker: perform K increments on random counters */
typedef struct {
    int thread_id;
    int iters;
    unsigned seed;
    uint64_t committed;
} worker_arg_t;

static void *worker(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    unsigned seed = wa->seed;
    uint64_t committed = 0;

    for (int i = 0; i < wa->iters; i++) {
        int idx = rand_r(&seed) % g_num_counters;
        int delta = (rand_r(&seed) % 10) + 1;

        for (;;) {
            tx_begin();
            uint64_t v = atomic_load(&g_counters[idx]);
            atomic_store(&g_counters[idx], v + delta);
            tx_end();
            break;  /* Success */
        }
        committed += delta;
    }
    wa->committed = committed;
    return NULL;
}

int main(int argc, char *argv[]) {
    int num_threads = argc > 1 ? atoi(argv[1]) : 2;
    int iters = argc > 2 ? atoi(argv[2]) : 100;
    g_num_counters = argc > 3 ? atoi(argv[3]) : 8;
    unsigned seed = argc > 4 ? atoi(argv[4]) : 42;

    printf("HTM Bench: %d threads, %d iters, %d counters\n",
           num_threads, iters, g_num_counters);

    g_counters = calloc(g_num_counters, sizeof(atomic_uint_least64_t));
    uint64_t initial = 0;
    for (int i = 0; i < g_num_counters; i++) {
        atomic_store(&g_counters[i], 1000);
        initial += 1000;
    }

    worker_arg_t *args = calloc(num_threads, sizeof(worker_arg_t));
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));

    for (int t = 0; t < num_threads; t++) {
        args[t].thread_id = t;
        args[t].iters = iters;
        args[t].seed = seed + t;
        pthread_create(&threads[t], NULL, worker, &args[t]);
    }

    uint64_t total_committed = 0;
    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
        total_committed += args[t].committed;
    }

    uint64_t final_sum = 0;
    for (int i = 0; i < g_num_counters; i++) {
        final_sum += atomic_load(&g_counters[i]);
    }

    uint64_t expected = initial + total_committed;

    int htm_ok = atomic_load(&g_htm_ok);
    int htm_fail = atomic_load(&g_htm_fail);
    int sgl = atomic_load(&g_sgl_fallback);

    printf("HTM: %d ok, %d fail, SGL: %d\n", htm_ok, htm_fail, sgl);
    printf("Sum: %llu, Expected: %llu\n",
           (unsigned long long)final_sum, (unsigned long long)expected);

    if (final_sum == expected) {
        printf("INVARIANT: PASS\n");
        free(args);
        free(threads);
        free(g_counters);
        return 0;
    }

    printf("INVARIANT: FAIL (diff=%lld)\n",
           (long long)(expected - final_sum));
    free(args);
    free(threads);
    free(g_counters);
    return 1;
}
