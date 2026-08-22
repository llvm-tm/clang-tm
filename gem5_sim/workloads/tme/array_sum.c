#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <arm_acle.h>

#include "../common/htm_bench.h"

#define ARRAY_SIZE 1024
#define NUM_TX 1000

int shared_array[ARRAY_SIZE];
struct htm_stats stats;

static void do_tx_work(void) {
    for (int t = 0; t < NUM_TX; t++) {
        stats.total_attempts++;
        uint64_t status = __tstart();
        if (status == 0) {
            for (int i = 0; i < ARRAY_SIZE; i++) {
                shared_array[i] += 1;
            }
            __tcommit();
            stats.commits++;
        } else {
            stats.aborts++;
        }
    }
}

int main(void) {
    htm_stats_init(&stats);
    do_tx_work();
    htm_stats_print(&stats);
    return stats.aborts > stats.commits ? 1 : 0;
}
