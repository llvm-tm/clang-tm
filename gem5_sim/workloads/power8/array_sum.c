#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "../common/htm_bench.h"

#define ARRAY_SIZE 1024
#define NUM_TX 1000

int shared_array[ARRAY_SIZE];
struct htm_stats stats;

static void do_tx_work(void) {
    for (int t = 0; t < NUM_TX; t++) {
        stats.total_attempts++;
        uint64_t cr;
        asm volatile(
            "tbegin.;"
            "beq   3f;"
            /* transactional region */
            : : : "memory"
        );
        for (int i = 0; i < ARRAY_SIZE; i++) {
            shared_array[i] += 1;
        }
        asm volatile("tend.;" : : : "memory");
        stats.commits++;
        asm volatile("b 4f;");
        /* abort handler */
        asm volatile("3:");
        stats.aborts++;
        /* fallthrough */
        asm volatile("4:");
    }
}

int main(void) {
    htm_stats_init(&stats);
    do_tx_work();
    htm_stats_print(&stats);
    return stats.aborts > stats.commits ? 1 : 0;
}
