#ifndef HTM_BENCH_H
#define HTM_BENCH_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum htm_result {
    HTM_COMMIT  = 0,
    HTM_ABORT   = 1,
    HTM_EXPLICIT = 2,
    HTM_CAPACITY = 3,
    HTM_CONFLICT = 4,
    HTM_OTHER    = 5,
};

struct htm_stats {
    uint64_t commits;
    uint64_t aborts;
    uint64_t explicit_aborts;
    uint64_t capacity_aborts;
    uint64_t conflict_aborts;
    uint64_t other_aborts;
    uint64_t total_attempts;
};

static inline void htm_stats_init(struct htm_stats *s) {
    memset(s, 0, sizeof(*s));
}

static inline void htm_stats_print(const struct htm_stats *s) {
    printf("=== HTM Benchmark Results ===\n");
    printf("Total attempts:  %lu\n", s->total_attempts);
    printf("Commits:         %lu (%.1f%%)\n",
           s->commits,
           100.0 * s->commits / s->total_attempts);
    printf("Aborts:          %lu (%.1f%%)\n",
           s->aborts,
           100.0 * s->aborts / s->total_attempts);
    if (s->aborts > 0) {
        printf("  Explicit:      %lu (%.1f%%)\n",
               s->explicit_aborts,
               100.0 * s->explicit_aborts / s->aborts);
        printf("  Capacity:      %lu (%.1f%%)\n",
               s->capacity_aborts,
               100.0 * s->capacity_aborts / s->aborts);
        printf("  Conflict:      %lu (%.1f%%)\n",
               s->conflict_aborts,
               100.0 * s->conflict_aborts / s->aborts);
        printf("  Other:         %lu (%.1f%%)\n",
               s->other_aborts,
               100.0 * s->other_aborts / s->aborts);
    }
}

#endif
