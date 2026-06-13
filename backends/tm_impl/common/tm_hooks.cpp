#include "tm_hooks.hpp"

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <thread>

// ── Shared per-thread TM state (used by all backends) ──────────
// Defined once here so that multiple backends can coexist without
// duplicate-symbol errors.
__thread int32_t    tm_nested_call_counter = 0;
__thread int32_t    tm_longjmp_ret = 0;
__thread sigjmp_buf tm_jmpbuf;

// ═══════════════════════════════════════════════════════════════
// Stub implementations — direct access, no TM overhead
// ═══════════════════════════════════════════════════════════════

extern "C" {

static void stub_begin() {}
static void stub_end()   {}

static void *stub_malloc(size_t s) {
    void *p = std::malloc(s);
    if (p) std::memset(p, 0, s);
    return p;
}
static void *stub_calloc(size_t n, size_t s) { return std::calloc(n, s); }
static void *stub_realloc(void *p, size_t s) { return std::realloc(p, s); }
static void  stub_free(void *p) { std::free(p); }

static uint8_t  stub_read_i1(uint8_t  *a) { return *a; }
static uint16_t stub_read_i2(uint16_t *a) { return *a; }
static uint32_t stub_read_i4(uint32_t *a) { return *a; }
static uint64_t stub_read_i8(uint64_t *a) { return *a; }
static float    stub_read_f4(float    *a) { return *a; }
static double   stub_read_f8(double   *a) { return *a; }
static void    *stub_read_ptr(void   **a) { return *a; }

static void stub_write_i1(uint8_t  *a, uint8_t  v) { *a = v; }
static void stub_write_i2(uint16_t *a, uint16_t v) { *a = v; }
static void stub_write_i4(uint32_t *a, uint32_t v) { *a = v; }
static void stub_write_i8(uint64_t *a, int64_t  v) { *a = (uint64_t)v; }
static void stub_write_f4(float    *a, float    v) { *a = v; }
static void stub_write_f8(double   *a, double   v) { *a = v; }
static void stub_write_ptr(void   **a, void    *v) { *a = v; }

} // extern "C"

// ═══════════════════════════════════════════════════════════════
// Public hook variables — default to stubs
// ═══════════════════════════════════════════════════════════════

extern "C" {

void     (*tm_begin)()       = stub_begin;
void     (*tm_end)()         = stub_end;
void    *(*tm_malloc)(size_t)         = stub_malloc;
void    *(*tm_calloc)(size_t, size_t) = stub_calloc;
void    *(*tm_realloc)(void*, size_t) = stub_realloc;
void     (*tm_free)(void*)            = stub_free;
uint8_t  (*tm_read_i1)(uint8_t*)      = stub_read_i1;
uint16_t (*tm_read_i2)(uint16_t*)     = stub_read_i2;
uint32_t (*tm_read_i4)(uint32_t*)     = stub_read_i4;
uint64_t (*tm_read_i8)(uint64_t*)     = stub_read_i8;
float    (*tm_read_f4)(float*)        = stub_read_f4;
double   (*tm_read_f8)(double*)       = stub_read_f8;
void    *(*tm_read_ptr)(void**)       = stub_read_ptr;
void     (*tm_write_i1)(uint8_t*, uint8_t)   = stub_write_i1;
void     (*tm_write_i2)(uint16_t*, uint16_t) = stub_write_i2;
void     (*tm_write_i4)(uint32_t*, uint32_t) = stub_write_i4;
void     (*tm_write_i8)(uint64_t*, int64_t)  = stub_write_i8;
void     (*tm_write_f4)(float*, float)        = stub_write_f4;
void     (*tm_write_f8)(double*, double)      = stub_write_f8;
void     (*tm_write_ptr)(void**, void*)        = stub_write_ptr;

} // extern "C"

// ═══════════════════════════════════════════════════════════════
// Thread counting + hook swap
// ═══════════════════════════════════════════════════════════════

static std::mutex s_hook_mutex;
static std::atomic<int> s_thread_count{1}; // main thread counts as 1
static TMRealHooks s_real_hooks;
static bool s_registered = false;

// Install hooks based on current thread count.
// Single-thread: stubs (direct access, no TM overhead).
// Multi-thread: real hooks registered by the backend.
// Must be called with s_hook_mutex held.
static void apply_hooks_unlocked() {
    bool single = s_thread_count.load() <= 1;
    const TMRealHooks *r = &s_real_hooks;
    auto pick = [&](auto stub, auto real) {
        return single ? stub : (real ? real : stub);
    };

    tm_begin    = pick(stub_begin,    r->begin);
    tm_end      = pick(stub_end,      r->end);
    tm_malloc   = pick(stub_malloc,   r->malloc);
    tm_calloc   = pick(stub_calloc,   r->calloc);
    tm_realloc  = pick(stub_realloc,  r->realloc);
    tm_free     = pick(stub_free,     r->free);
    tm_read_i1  = pick(stub_read_i1,  r->read_i1);
    tm_read_i2  = pick(stub_read_i2,  r->read_i2);
    tm_read_i4  = pick(stub_read_i4,  r->read_i4);
    tm_read_i8  = pick(stub_read_i8,  r->read_i8);
    tm_read_f4  = pick(stub_read_f4,  r->read_f4);
    tm_read_f8  = pick(stub_read_f8,  r->read_f8);
    tm_read_ptr = pick(stub_read_ptr, r->read_ptr);
    tm_write_i1 = pick(stub_write_i1, r->write_i1);
    tm_write_i2 = pick(stub_write_i2, r->write_i2);
    tm_write_i4 = pick(stub_write_i4, r->write_i4);
    tm_write_i8 = pick(stub_write_i8, r->write_i8);
    tm_write_f4 = pick(stub_write_f4, r->write_f4);
    tm_write_f8 = pick(stub_write_f8, r->write_f8);
    tm_write_ptr= pick(stub_write_ptr, r->write_ptr);
}

void tm_register_real_hooks(const TMRealHooks *hooks) {
    std::lock_guard<std::mutex> lock(s_hook_mutex);
    s_real_hooks = *hooks;
    s_registered = true;
    apply_hooks_unlocked();
}

void tm_set_num_threads(int n) {
    std::lock_guard<std::mutex> lock(s_hook_mutex);
    s_thread_count.store(n);
    if (s_registered)
        apply_hooks_unlocked();
}

// Called by each backend's tm_init_thread().
// Increments the thread count. If the count exceeds 1 (multi-thread),
// atomically swaps to real hooks under the global lock.
void tm_hook_init_thread() {
    int prev = s_thread_count.fetch_add(1);
    if (prev >= 1 && s_registered) {
        // We're transitioning to multi-threaded: lock and install real hooks.
        // All threads currently executing TM operations will see the new
        // hooks on their next TM call (since each call loads the pointer).
        std::lock_guard<std::mutex> lock(s_hook_mutex);
        apply_hooks_unlocked();
    }
}

void tm_hook_exit_thread() {
    s_thread_count.fetch_sub(1);
}

const TMRealHooks *tm_get_real_hooks() {
    return &s_real_hooks;
}

// Phase-based TM: atomically swap ALL hooks under the global lock.
// This allows the runtime to switch backends at runtime based on
// abort rate, workload characteristics, etc.
void tm_swap_runtime(const TMRealHooks *hooks) {
    std::lock_guard<std::mutex> lock(s_hook_mutex);
    if (hooks->begin)   tm_begin    = hooks->begin;
    if (hooks->end)     tm_end      = hooks->end;
    if (hooks->malloc)  tm_malloc   = hooks->malloc;
    if (hooks->calloc)  tm_calloc   = hooks->calloc;
    if (hooks->realloc) tm_realloc  = hooks->realloc;
    if (hooks->free)    tm_free     = hooks->free;
    if (hooks->read_i1) tm_read_i1  = hooks->read_i1;
    if (hooks->read_i2) tm_read_i2  = hooks->read_i2;
    if (hooks->read_i4) tm_read_i4  = hooks->read_i4;
    if (hooks->read_i8) tm_read_i8  = hooks->read_i8;
    if (hooks->read_f4) tm_read_f4  = hooks->read_f4;
    if (hooks->read_f8) tm_read_f8  = hooks->read_f8;
    if (hooks->read_ptr) tm_read_ptr = hooks->read_ptr;
    if (hooks->write_i1) tm_write_i1 = hooks->write_i1;
    if (hooks->write_i2) tm_write_i2 = hooks->write_i2;
    if (hooks->write_i4) tm_write_i4 = hooks->write_i4;
    if (hooks->write_i8) tm_write_i8 = hooks->write_i8;
    if (hooks->write_f4) tm_write_f4 = hooks->write_f4;
    if (hooks->write_f8) tm_write_f8 = hooks->write_f8;
    if (hooks->write_ptr) tm_write_ptr = hooks->write_ptr;
    // Also update s_real_hooks so tm_hook_init_thread() doesn't revert
    if (hooks->begin)   s_real_hooks.begin   = hooks->begin;
    if (hooks->end)     s_real_hooks.end     = hooks->end;
    if (hooks->malloc)  s_real_hooks.malloc  = hooks->malloc;
    if (hooks->calloc)  s_real_hooks.calloc  = hooks->calloc;
    if (hooks->realloc) s_real_hooks.realloc = hooks->realloc;
    if (hooks->free)    s_real_hooks.free    = hooks->free;
    if (hooks->read_i1) s_real_hooks.read_i1 = hooks->read_i1;
    if (hooks->read_i2) s_real_hooks.read_i2 = hooks->read_i2;
    if (hooks->read_i4) s_real_hooks.read_i4 = hooks->read_i4;
    if (hooks->read_i8) s_real_hooks.read_i8 = hooks->read_i8;
    if (hooks->read_f4) s_real_hooks.read_f4 = hooks->read_f4;
    if (hooks->read_f8) s_real_hooks.read_f8 = hooks->read_f8;
    if (hooks->read_ptr) s_real_hooks.read_ptr = hooks->read_ptr;
    if (hooks->write_i1) s_real_hooks.write_i1 = hooks->write_i1;
    if (hooks->write_i2) s_real_hooks.write_i2 = hooks->write_i2;
    if (hooks->write_i4) s_real_hooks.write_i4 = hooks->write_i4;
    if (hooks->write_i8) s_real_hooks.write_i8 = hooks->write_i8;
    if (hooks->write_f4) s_real_hooks.write_f4 = hooks->write_f4;
    if (hooks->write_f8) s_real_hooks.write_f8 = hooks->write_f8;
    if (hooks->write_ptr) s_real_hooks.write_ptr = hooks->write_ptr;
}

// ═══════════════════════════════════════════════════════════════
// Trace infrastructure (used by both explicit API and plugin pipeline)
// ═══════════════════════════════════════════════════════════════

// Weak default for tm_trace — overridden by tm_trace_runtime.cpp when linked
extern "C" __attribute__((weak)) void tm_trace(uint32_t, void*, uint64_t, uint64_t) {}

// Thread-local sequence counter for ordering
static thread_local uint64_t t_trace_seq = 0;

// Helper: write a text-format trace line to a FILE*
static FILE *g_trace_file = nullptr;
static std::atomic<uint64_t> g_trace_ts{0};

// ── Trace helper: write a formatted line ───────────────────────────────
static void trace_write_line(FILE *f, uint64_t ts, uint64_t tid, int type,
                             uint64_t addr, int width, uint64_t val) {
    fprintf(f, "%llu %llu %d 0x%llx %d 0x%llx\n",
            (unsigned long long)ts, (unsigned long long)tid, type,
            (unsigned long long)addr, width, (unsigned long long)val);
}

// Individual trace wrapper functions (no templates to avoid type mismatch)
extern "C" {

static void trace_begin() {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 2, 0, 0, 0);
    }
    if (s_real_hooks.begin) s_real_hooks.begin();
}

static void trace_end() {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 3, 0, 0, 0);
    }
    if (s_real_hooks.end) s_real_hooks.end();
}

static void *trace_malloc(size_t s) {
    void *p = s_real_hooks.malloc ? s_real_hooks.malloc(s) : std::malloc(s);
    if (g_trace_file && p) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 4, (uint64_t)(uintptr_t)p, (int)s, 0);
    }
    return p;
}

static void *trace_calloc(size_t n, size_t s) {
    void *p = s_real_hooks.calloc ? s_real_hooks.calloc(n, s) : std::calloc(n, s);
    if (g_trace_file && p) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 4, (uint64_t)(uintptr_t)p, (int)(n * s), 0);
    }
    return p;
}

static void *trace_realloc(void *old, size_t s) {
    void *p = s_real_hooks.realloc ? s_real_hooks.realloc(old, s) : std::realloc(old, s);
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        if (old && p != old) {
            trace_write_line(g_trace_file, ts, tid, 5, (uint64_t)(uintptr_t)old, 0, 0);
        }
        if (p) {
            ts = g_trace_ts.fetch_add(1);
            trace_write_line(g_trace_file, ts, tid, 4, (uint64_t)(uintptr_t)p, (int)s, 0);
        }
    }
    return p;
}

static void trace_free(void *p) {
    if (g_trace_file && p) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 5, (uint64_t)(uintptr_t)p, 0, 0);
    }
    if (s_real_hooks.free) s_real_hooks.free(p);
    else if (p) std::free(p);
}

static uint8_t trace_read_i1(uint8_t *a) {
    uint8_t r = s_real_hooks.read_i1 ? s_real_hooks.read_i1(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 1, r);
    }
    return r;
}
static uint16_t trace_read_i2(uint16_t *a) {
    uint16_t r = s_real_hooks.read_i2 ? s_real_hooks.read_i2(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 2, r);
    }
    return r;
}
static uint32_t trace_read_i4(uint32_t *a) {
    uint32_t r = s_real_hooks.read_i4 ? s_real_hooks.read_i4(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 4, r);
    }
    return r;
}
static uint64_t trace_read_i8(uint64_t *a) {
    uint64_t r = s_real_hooks.read_i8 ? s_real_hooks.read_i8(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 8, r);
    }
    return r;
}
static float trace_read_f4(float *a) {
    float r = s_real_hooks.read_f4 ? s_real_hooks.read_f4(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        uint32_t tmp; memcpy(&tmp, &r, 4);
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 4, tmp);
    }
    return r;
}
static double trace_read_f8(double *a) {
    double r = s_real_hooks.read_f8 ? s_real_hooks.read_f8(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        uint64_t tmp; memcpy(&tmp, &r, 8);
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 8, tmp);
    }
    return r;
}
static void *trace_read_ptr(void **a) {
    void *r = s_real_hooks.read_ptr ? s_real_hooks.read_ptr(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 0, (uint64_t)(uintptr_t)a, 8, (uint64_t)(uintptr_t)r);
    }
    return r;
}

static void trace_write_i1(uint8_t *a, uint8_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 1, v);
    }
    if (s_real_hooks.write_i1) s_real_hooks.write_i1(a, v);
}
static void trace_write_i2(uint16_t *a, uint16_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 2, v);
    }
    if (s_real_hooks.write_i2) s_real_hooks.write_i2(a, v);
}
static void trace_write_i4(uint32_t *a, uint32_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 4, v);
    }
    if (s_real_hooks.write_i4) s_real_hooks.write_i4(a, v);
}
static void trace_write_i8(uint64_t *a, int64_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 8, (uint64_t)v);
    }
    if (s_real_hooks.write_i8) s_real_hooks.write_i8(a, v);
}
static void trace_write_f4(float *a, float v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        uint32_t tmp; memcpy(&tmp, &v, 4);
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 4, tmp);
    }
    if (s_real_hooks.write_f4) s_real_hooks.write_f4(a, v);
}
static void trace_write_f8(double *a, double v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        uint64_t tmp; memcpy(&tmp, &v, 8);
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 8, tmp);
    }
    if (s_real_hooks.write_f8) s_real_hooks.write_f8(a, v);
}
static void trace_write_ptr(void **a, void *v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line(g_trace_file, ts, tid, 1, (uint64_t)(uintptr_t)a, 8, (uint64_t)(uintptr_t)v);
    }
    if (s_real_hooks.write_ptr) s_real_hooks.write_ptr(a, v);
}

} // extern "C"

// Install trace wrappers when TM_TRACE_FILE env var is set.
// Called automatically at startup via constructor attribute.
__attribute__((constructor)) static void tm_trace_hook_init() {
    const char *path = getenv("TM_TRACE_PATH");
    if (!path || !path[0])
        return;
    g_trace_file = fopen(path, "w");
    if (!g_trace_file) {
        fprintf(stderr, "tm_hooks: cannot open trace '%s'\n", path);
        return;
    }

    // Save current hooks as real hooks (they were set by backend init)
    s_real_hooks.begin   = tm_begin;
    s_real_hooks.end     = tm_end;
    s_real_hooks.malloc  = tm_malloc;
    s_real_hooks.calloc  = tm_calloc;
    s_real_hooks.realloc = tm_realloc;
    s_real_hooks.free    = tm_free;
    s_real_hooks.read_i1 = tm_read_i1;
    s_real_hooks.read_i2 = tm_read_i2;
    s_real_hooks.read_i4 = tm_read_i4;
    s_real_hooks.read_i8 = tm_read_i8;
    s_real_hooks.read_f4 = tm_read_f4;
    s_real_hooks.read_f8 = tm_read_f8;
    s_real_hooks.read_ptr = tm_read_ptr;
    s_real_hooks.write_i1 = tm_write_i1;
    s_real_hooks.write_i2 = tm_write_i2;
    s_real_hooks.write_i4 = tm_write_i4;
    s_real_hooks.write_i8 = tm_write_i8;
    s_real_hooks.write_f4 = tm_write_f4;
    s_real_hooks.write_f8 = tm_write_f8;
    s_real_hooks.write_ptr = tm_write_ptr;

    // Replace hooks with trace wrappers
    tm_begin    = trace_begin;
    tm_end      = trace_end;
    tm_malloc   = trace_malloc;
    tm_calloc   = trace_calloc;
    tm_realloc  = trace_realloc;
    tm_free     = trace_free;
    tm_read_i1  = trace_read_i1;
    tm_read_i2  = trace_read_i2;
    tm_read_i4  = trace_read_i4;
    tm_read_i8  = trace_read_i8;
    tm_read_f4  = trace_read_f4;
    tm_read_f8  = trace_read_f8;
    tm_read_ptr = trace_read_ptr;
    tm_write_i1 = trace_write_i1;
    tm_write_i2 = trace_write_i2;
    tm_write_i4 = trace_write_i4;
    tm_write_i8 = trace_write_i8;
    tm_write_f4 = trace_write_f4;
    tm_write_f8 = trace_write_f8;
    tm_write_ptr= trace_write_ptr;
}

__attribute__((destructor)) static void tm_trace_hook_fini() {
    if (g_trace_file) {
        fflush(g_trace_file);
        if (g_trace_file != stderr && g_trace_file != stdout)
            fclose(g_trace_file);
        g_trace_file = nullptr;
    }
}
