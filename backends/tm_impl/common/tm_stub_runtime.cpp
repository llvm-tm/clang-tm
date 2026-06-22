// ── Stub runtime for baseline computation timing ──────────
// TM operations are no-ops / direct access. TxBegin/TxEnd
// are instrumented with wall-clock timestamps to capture
// per-transaction computation cost without TM overhead.
// Output goes to TM_BASELINE_PATH (one line per TX):
//   thread_id seq begin_ns end_ns
// Final line: "TOTAL: N seqs sum_ns"
//
// NOTES:
//   tm_api.hpp declares most TM operations as function-pointer DATA symbols
//   (e.g. extern void (*tm_begin)()).  Only tm_init/tm_exit/tm_init_thread/
//   tm_exit_thread are plain TEXT functions.  This file defines stubs as
//   static C functions and assigns them to the function-pointer globals.

#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

static FILE *g_bl = nullptr;
static std::mutex g_bl_mtx;
static long long g_tx_seqs = 0;
static long long g_total_ns = 0;

static long long now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static void bl_open() {
    if (g_bl) return;
    const char *path = getenv("TM_BASELINE_PATH");
    if (!path) return;
    g_bl = fopen(path, "w");
    if (!g_bl) {
        fprintf(stderr, "tm_baseline: cannot open %s\n", path);
    }
}

static void bl_write(long long tid, long long seq, long long begin_ns, long long end_ns) {
    if (!g_bl) return;
    std::lock_guard<std::mutex> lock(g_bl_mtx);
    fprintf(g_bl, "%lld %lld %lld %lld\n", tid, seq, begin_ns, end_ns);
    g_total_ns += end_ns - begin_ns;
    g_tx_seqs++;
}

static __thread long long g_begin_ns = 0;
static __thread long long g_tid = 0;

// ── Private stub implementations (static, C linkage) ──────

extern "C" {

static void stub_init() { bl_open(); }
static void stub_exit() {
    bl_open();
    if (g_bl) {
        fprintf(g_bl, "TOTAL: %lld seqs %lld ns\n", g_tx_seqs, g_total_ns);
        fclose(g_bl);
        g_bl = nullptr;
    }
}
static void stub_init_thread() {
    static std::atomic<long long> next_tid{1};
    g_tid = next_tid.fetch_add(1, std::memory_order_relaxed);
}
static void stub_exit_thread() {}
static void stub_begin() { g_begin_ns = now_ns(); }
static void stub_end() {
    long long end_ns = now_ns();
    bl_write(g_tid, g_tx_seqs + 1, g_begin_ns, end_ns);
}
static void stub_abort() {}
static void *stub_malloc(size_t s) { return std::malloc(s); }
static void *stub_calloc(size_t n, size_t s) { return std::calloc(n, s); }
static void *stub_realloc(void *p, size_t s) { return std::realloc(p, s); }
static void  stub_free(void *p) { std::free(p); }
static uint8_t  stub_read_i1(const uint8_t  *a) { return *a; }
static uint16_t stub_read_i2(const uint16_t *a) { return *a; }
static uint32_t stub_read_i4(const uint32_t *a) { return *a; }
static uint64_t stub_read_i8(const uint64_t *a) { return *a; }
static float    stub_read_f4(const float    *a) { return *a; }
static double   stub_read_f8(const double   *a) { return *a; }
static void    *stub_read_ptr(void * const *a)  { return *a; }
static void stub_write_i1(uint8_t  *a, uint8_t  v) { *a = v; }
static void stub_write_i2(uint16_t *a, uint16_t v) { *a = v; }
static void stub_write_i4(uint32_t *a, uint32_t v) { *a = v; }
static void stub_write_i8(uint64_t *a, int64_t  v) { *a = (uint64_t)v; }
static void stub_write_f4(float    *a, float    v) { *a = v; }
static void stub_write_f8(double   *a, double   v) { *a = v; }
static void stub_write_ptr(void   **a, void    *v) { *a = v; }
static void *stub_get_env() { return nullptr; }
static void  stub_set_jmpbuf(void *) {}
static void *stub_get_thread_state() { return nullptr; }
static void  stub_trace(uint32_t, void*, uint64_t, uint64_t) {}

// ── Plain TEXT functions (benchmarks call these directly) ─
void tm_init() { bl_open(); }
void tm_exit() {
    bl_open();
    if (g_bl) {
        fprintf(g_bl, "TOTAL: %lld seqs %lld ns\n", g_tx_seqs, g_total_ns);
        fclose(g_bl);
        g_bl = nullptr;
    }
}
void tm_init_thread() {
    static std::atomic<long long> next_tid{1};
    g_tid = next_tid.fetch_add(1, std::memory_order_relaxed);
}
void tm_exit_thread() {}

// ── Function-pointer DATA symbols ─────────────────────────
// tm_api.hpp declares these as 'extern void (*tm_begin)()' etc.
void (*tm_begin)()        = stub_begin;
void (*tm_end)()          = stub_end;
void (*tm_abort)()        = stub_abort;
void *(*tm_malloc)(size_t)          = stub_malloc;
void *(*tm_calloc)(size_t, size_t)  = stub_calloc;
void *(*tm_realloc)(void*, size_t)  = stub_realloc;
void  (*tm_free)(void*)             = stub_free;

uint8_t  (*tm_read_i1)(const uint8_t*)   = stub_read_i1;
uint16_t (*tm_read_i2)(const uint16_t*)  = stub_read_i2;
uint32_t (*tm_read_i4)(const uint32_t*)  = stub_read_i4;
uint64_t (*tm_read_i8)(const uint64_t*)  = stub_read_i8;
float    (*tm_read_f4)(const float*)     = stub_read_f4;
double   (*tm_read_f8)(const double*)    = stub_read_f8;
void    *(*tm_read_ptr)(void* const*)    = stub_read_ptr;

void (*tm_write_i1)(uint8_t*, uint8_t)           = stub_write_i1;
void (*tm_write_i2)(uint16_t*, uint16_t)         = stub_write_i2;
void (*tm_write_i4)(uint32_t*, uint32_t)         = stub_write_i4;
void (*tm_write_i8)(uint64_t*, int64_t)          = stub_write_i8;
void (*tm_write_f4)(float*, float)               = stub_write_f4;
void (*tm_write_f8)(double*, double)             = stub_write_f8;
void (*tm_write_ptr)(void**, void*)              = stub_write_ptr;

void *(*tm_get_env)()                  = stub_get_env;
void  (*tm_set_jmpbuf)(void*)         = stub_set_jmpbuf;
void *(*tm_get_thread_state)()        = stub_get_thread_state;
void  (*tm_trace)(uint32_t, void*, uint64_t, uint64_t) = stub_trace;

__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
__thread sigjmp_buf tm_jmpbuf;

} // extern "C"
