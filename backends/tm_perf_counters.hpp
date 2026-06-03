#ifndef TM_PERF_COUNTERS_HPP
#define TM_PERF_COUNTERS_HPP

/// Performance counters for queue dispatch profiling.
///
/// Activation: compile queue_runtime.cpp with `-DTM_PERF_COUNTERS`.
/// Removal: undefine the macro — all macros become no-ops.
///
/// Usage in queue_runtime.cpp:
///
///   // Declare counter set for a thread (in class field or global):
///   TmPerfCounters my_perf;
///
///   // Register for dump output (call once at init):
///   TM_PERF_REGISTER(&my_perf);
///
///   // Increment a counter field:
///   TM_PERF_INC(my_perf, queue_execs);
///
///   // Set a field to a value:
///   TM_PERF_SET(my_perf, t_start, value);
///
///   // Capture start-of-activity timestamp (first-call-only):
///   TM_PERF_BEGIN(my_perf);
///
///   // Timestamp end of activity:
///   TM_PERF_END(my_perf);
///
///   // Time a scope and accumulate into a field:
///   TM_PERF_SCOPE(my_perf, exec_ns) {
///       // ... work ...
///   }

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

// =========================================================================
// Counter set
// =========================================================================

struct alignas(64) TmPerfCounters {
    char name[32] = {0};
    int64_t t_start = 0;
    int64_t t_end = 0;

    int64_t enqueue_calls = 0;
    int64_t wait_calls = 0;
    int64_t inline_execs = 0;
    int64_t queue_execs = 0;
    int64_t worker_tasks = 0;
    int64_t worker_spins = 0;

    int64_t alloc_ns = 0;
    int64_t push_ns = 0;
    int64_t worker_wait_ns = 0;
    int64_t exec_ns = 0;
    int64_t wait_block_ns = 0;
};

// =========================================================================
// Low-level functions (always compiled)
// =========================================================================

inline int64_t tm_perf_now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// =========================================================================
// Macros
// =========================================================================

#ifdef TM_PERF_COUNTERS

#define TM_PERF_INC(c, f)             do { ++(c).f; } while(0)
#define TM_PERF_SET(c, f, v)          do { (c).f = (v); } while(0)
#define TM_PERF_ADD(c, f, v)          do { (c).f += (v); } while(0)

#define TM_PERF_BEGIN(c)              do { if (!(c).t_start) (c).t_start = tm_perf_now(); } while(0)
#define TM_PERF_END(c)                do { (c).t_end = tm_perf_now(); } while(0)

// Time one scope, accumulate into (c).field.
// Uses a for-loop whose body runs exactly once.
#define TM_PERF_SCOPE(c, field)       \
    for (int64_t _p_t0 = tm_perf_now(), _p_flag = 1; _p_flag; _p_flag = 0, (c).field += tm_perf_now() - _p_t0)

#define TM_PERF_REGISTER(c)           do { tm_perf_registry().all.push_back((c)); } while(0)

#else // !TM_PERF_COUNTERS

#define TM_PERF_INC(c, f)             ((void)0)
#define TM_PERF_SET(c, f, v)          ((void)0)
#define TM_PERF_ADD(c, f, v)          ((void)0)
#define TM_PERF_BEGIN(c)              ((void)0)
#define TM_PERF_END(c)                ((void)0)
#define TM_PERF_SCOPE(c, field)       if (true)
#define TM_PERF_REGISTER(c)           ((void)0)

#endif

// =========================================================================
// Registry
// =========================================================================

struct TmPerfRegistry {
    std::vector<TmPerfCounters *> all;
};

inline TmPerfRegistry &tm_perf_registry()
{
    static TmPerfRegistry reg;
    return reg;
}

// =========================================================================
// Dump
// =========================================================================

inline void tm_perf_dump()
{
#ifdef TM_PERF_COUNTERS
    auto &reg = tm_perf_registry();
    int64_t e = 0, i = 0, q = 0;
    int64_t an = 0, pn = 0, en = 0, wn = 0;
    int64_t wt = 0, ws = 0;
    double wall_s = 0;

    fprintf(stderr, "\n--- TM Perf Counters ---\n");
    fprintf(stderr, "%-18s %8s %8s %8s %8s %8s %8s %8s %8s\n",
            "Thread", "Enq", "Inline", "Queue",
            "Alloc", "Push", "Exec", "WBlk", "Tasks");
    fprintf(stderr, "---------------------------------------------------------\n");
    for (auto *c : reg.all) {
        double d = (c->t_end - c->t_start) / 1e9;
        if (d > wall_s) wall_s = d;
        fprintf(stderr, "%-18s %8lld %8lld %8lld %8lld %8lld %8lld %8lld %8lld\n",
                c->name,
                (long long)c->enqueue_calls,
                (long long)c->inline_execs,
                (long long)c->queue_execs,
                (long long)c->alloc_ns,
                (long long)c->push_ns,
                (long long)c->exec_ns,
                (long long)c->wait_block_ns,
                (long long)c->worker_tasks);
        e += c->enqueue_calls;
        i += c->inline_execs;
        q += c->queue_execs;
        an += c->alloc_ns;
        pn += c->push_ns;
        en += c->exec_ns;
        wn += c->wait_block_ns;
        wt += c->worker_tasks;
        ws += c->worker_spins;
    }
    fprintf(stderr, "---------------------------------------------------------\n");
    fprintf(stderr, "%-18s %8lld %8lld %8lld %8lld %8lld %8lld %8lld %8lld\n",
            "TOTAL", (long long)e, (long long)i, (long long)q,
            (long long)an, (long long)pn, (long long)en, (long long)wn, (long long)wt);

    fprintf(stderr, "\n");
    fprintf(stderr, "Wall: %.3f s  Rate: %.0f tx/s\n", wall_s, wall_s > 0 ? q / wall_s : 0);
    if (q > 0) {
        fprintf(stderr, "Avg: alloc=%.0f push=%.0f exec=%.0f wait=%.0f  overhead=%.0f ns\n",
                (double)an / q, (double)pn / q, (double)en / q, (double)wn / q,
                (double)(an + pn + wn) / q);
    }
    if (ws > 0) fprintf(stderr, "Worker spins: %lld\n", (long long)ws);
    if (wt > 0) fprintf(stderr, "Worker idle:  %.3f s\n", (double)wt / 1e9);
#endif
}

#endif // TM_PERF_COUNTERS_HPP
