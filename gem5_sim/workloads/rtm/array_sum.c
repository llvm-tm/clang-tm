#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>
#include <rtmintrin.h>

#include "../common/htm_bench.h"

#define ARRAY_SIZE 1024
#define NUM_TX 1000

int shared_array[ARRAY_SIZE];
struct htm_stats stats;

static void do_tx_work(void) {
    for (int t = 0; t < NUM_TX; t++) {
        stats.total_attempts++;
        unsigned status = _xbegin();
        if (status == _XBEGIN_STARTED) {
            for (int i = 0; i < ARRAY_SIZE; i++) {
                shared_array[i] += 1;
            }
            _xend();
            stats.commits++;
        } else {
            stats.aborts++;
            if (status & _XABORT_EXPLICIT)
                stats.explicit_aborts++;
            else if (status & _XABORT_CAPACITY)
                stats.capacity_aborts++;
            else if (status & _XABORT_CONFLICT)
                stats.conflict_aborts++;
            else
                stats.other_aborts++;
        }
    }
}

int main(void) {
    htm_stats_init(&stats);
    do_tx_work();
    htm_stats_print(&stats);
    return stats.aborts > stats.commits ? 1 : 0;
}
