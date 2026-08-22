#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <arm_acle.h>

#include "../common/htm_bench.h"

#define NUM_BUCKETS 64
#define NUM_KEYS 512
#define NUM_THREADS 4
#define NUM_TX 500

struct entry {
    int key;
    int value;
    struct entry *next;
};

struct bucket {
    struct entry *head;
};

struct bucket table[NUM_BUCKETS];
struct htm_stats stats[NUM_THREADS];

static int hash(int key) {
    return (unsigned)key % NUM_BUCKETS;
}

static void insert_atomic(int key, int value) {
    int b = hash(key);
    struct entry *e = malloc(sizeof(*e));
    e->key = key;
    e->value = value;
    e->next = table[b].head;
    table[b].head = e;
}

static void lookup_tx(int key, int tid) {
    for (int t = 0; t < NUM_TX; t++) {
        stats[tid].total_attempts++;
        uint64_t status = __tstart();
        if (status == 0) {
            int b = hash(key);
            int found = 0;
            for (struct entry *e = table[b].head; e; e = e->next) {
                if (e->key == key) {
                    found = 1;
                    break;
                }
            }
            __tcommit();
            stats[tid].commits++;
        } else {
            stats[tid].aborts++;
        }
    }
}

void *worker(void *arg) {
    int tid = (uintptr_t)arg;
    lookup_tx(42, tid);
    return NULL;
}

int main(void) {
    for (int i = 0; i < NUM_THREADS; i++)
        htm_stats_init(&stats[i]);

    for (int i = 0; i < NUM_KEYS; i++)
        insert_atomic(i, i * 10);

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    struct htm_stats total;
    htm_stats_init(&total);
    for (int i = 0; i < NUM_THREADS; i++) {
        total.total_attempts  += stats[i].total_attempts;
        total.commits         += stats[i].commits;
        total.aborts          += stats[i].aborts;
        total.explicit_aborts += stats[i].explicit_aborts;
        total.capacity_aborts += stats[i].capacity_aborts;
        total.conflict_aborts += stats[i].conflict_aborts;
        total.other_aborts    += stats[i].other_aborts;
    }
    htm_stats_print(&total);
    return 0;
}
