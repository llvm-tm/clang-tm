#include "tm_hooks.hpp"

#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <setjmp.h>

// ── Shared per-thread TM state (used by all backends) ──────────
__thread int32_t    tm_nested_call_counter = 0;
__thread int32_t    tm_longjmp_ret = 0;
__thread sigjmp_buf tm_jmpbuf;

// ═══════════════════════════════════════════════════════════════
// Stub implementations — direct access, no TM overhead.
// Always compiled (static = internal linkage, no linker conflict).
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

static void *stub_tm_get_env() {
    return (void*)&tm_jmpbuf;
}

static void stub_tm_set_jmpbuf(void *buf) {
    (void)buf;
}

static void *stub_tm_get_thread_state() {
    return (void*)&tm_nested_call_counter;
}

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
#if defined(__APPLE__)
int      (*tm_sigsetjmp)(void*, int)            = (int(*)(void*, int))sigsetjmp;
#else
// glibc: sigsetjmp is a macro expanding to __sigsetjmp(env, savemask);
// taking its address requires the underlying function name.
int      (*tm_sigsetjmp)(void*, int)            = (int(*)(void*, int))__sigsetjmp;
#endif
void    *(*tm_get_env)()                        = stub_tm_get_env;
void     (*tm_set_jmpbuf)(void*)               = stub_tm_set_jmpbuf;
void *(*tm_get_thread_state)()          = stub_tm_get_thread_state;

// Init/exit hooks — defined as DATA variables so the LLVM pass can load
// through them.  Only compiled under -DLLVM_TM_PLUGIN; without the plugin
// the backend provides strong TEXT (function) definitions directly.
// Weak linkage so plugin builds can override with strong DATA variables.
#ifdef LLVM_TM_PLUGIN
__attribute__((weak)) void (*tm_init)()        = stub_begin;
__attribute__((weak)) void (*tm_exit)()        = stub_end;
__attribute__((weak)) void (*tm_init_thread)() = stub_begin;
__attribute__((weak)) void (*tm_exit_thread)() = stub_end;
#endif

// Weak default for tm_trace — the real implementation is in tm_trace_runtime.cpp
__attribute__((weak)) void tm_trace_stub(uint32_t, void*, uint64_t, uint64_t) {}
__attribute__((weak)) void (*tm_trace)(uint32_t, void*, uint64_t, uint64_t) = tm_trace_stub;

} // extern "C"

// ═══════════════════════════════════════════════════════════════
// Thread counting + hook swap
// ═══════════════════════════════════════════════════════════════

static std::mutex s_hook_mutex;
static std::atomic<int> s_thread_count{1}; // main thread counts as 1
static TMRealHooks s_real_hooks;
static bool s_registered = false;
static bool g_trace_active = false;
static TMRealHooks s_trace_real_hooks;

// Install hooks based on current thread count.
// Single-thread: stubs (direct access, no TM overhead).
// Multi-thread: real hooks registered by the backend.
// Must be called with s_hook_mutex held.
//
// Under LLVM_TM_PLUGIN the instrumentation always emits TM operations
// through function pointers (tm_read_i4, tm_begin, etc.), so stubs
// would silently no-op transactions.  Force real hooks always.
static void apply_hooks_unlocked() {
    bool single =
#ifdef LLVM_TM_PLUGIN
        false;
#else
        s_thread_count.load() <= 1;
#endif
    if (g_trace_active) {
        // Trace wrappers are active: update the trace's saved real hooks
        // and keep the trace wrappers in place.  The delegation chain is:
        //   benchmark → trace_begin → s_trace_real_hooks.begin (= real backend)
        s_trace_real_hooks.begin   = s_real_hooks.begin;
        s_trace_real_hooks.end     = s_real_hooks.end;
        s_trace_real_hooks.malloc  = s_real_hooks.malloc;
        s_trace_real_hooks.calloc  = s_real_hooks.calloc;
        s_trace_real_hooks.realloc = s_real_hooks.realloc;
        s_trace_real_hooks.free    = s_real_hooks.free;
        s_trace_real_hooks.read_i1 = s_real_hooks.read_i1;
        s_trace_real_hooks.read_i2 = s_real_hooks.read_i2;
        s_trace_real_hooks.read_i4 = s_real_hooks.read_i4;
        s_trace_real_hooks.read_i8 = s_real_hooks.read_i8;
        s_trace_real_hooks.read_f4 = s_real_hooks.read_f4;
        s_trace_real_hooks.read_f8 = s_real_hooks.read_f8;
        s_trace_real_hooks.read_ptr = s_real_hooks.read_ptr;
        s_trace_real_hooks.write_i1 = s_real_hooks.write_i1;
        s_trace_real_hooks.write_i2 = s_real_hooks.write_i2;
        s_trace_real_hooks.write_i4 = s_real_hooks.write_i4;
        s_trace_real_hooks.write_i8 = s_real_hooks.write_i8;
        s_trace_real_hooks.write_f4 = s_real_hooks.write_f4;
        s_trace_real_hooks.write_f8 = s_real_hooks.write_f8;
        s_trace_real_hooks.write_ptr = s_real_hooks.write_ptr;
        s_trace_real_hooks.get_env = s_real_hooks.get_env;
        s_trace_real_hooks.set_jmpbuf = s_real_hooks.set_jmpbuf;
        s_trace_real_hooks.get_thread_state = s_real_hooks.get_thread_state;
        // Keep tm_begin/end etc. set to trace wrappers (already installed by
        // tm_trace_hook_init constructor).  s_trace_real_hooks now points
        // to the real backend, so trace wrappers will delegate correctly.
    } else {
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
        tm_get_env = pick(stub_tm_get_env, r->get_env);
        tm_set_jmpbuf = pick(stub_tm_set_jmpbuf, r->set_jmpbuf);
        tm_get_thread_state = pick(stub_tm_get_thread_state, r->get_thread_state);
    }
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
    if (hooks->get_env) tm_get_env = hooks->get_env;
    if (hooks->set_jmpbuf) tm_set_jmpbuf = hooks->set_jmpbuf;
    if (hooks->get_thread_state) tm_get_thread_state = hooks->get_thread_state;
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
    if (hooks->get_env) s_real_hooks.get_env = hooks->get_env;
    if (hooks->set_jmpbuf) s_real_hooks.set_jmpbuf = hooks->set_jmpbuf;
    if (hooks->write_i2) s_real_hooks.write_i2 = hooks->write_i2;
    if (hooks->write_i4) s_real_hooks.write_i4 = hooks->write_i4;
    if (hooks->write_i8) s_real_hooks.write_i8 = hooks->write_i8;
    if (hooks->write_f4) s_real_hooks.write_f4 = hooks->write_f4;
    if (hooks->write_f8) s_real_hooks.write_f8 = hooks->write_f8;
    if (hooks->write_ptr) s_real_hooks.write_ptr = hooks->write_ptr;
    if (hooks->get_thread_state) s_real_hooks.get_thread_state = hooks->get_thread_state;
}

// ═══════════════════════════════════════════════════════════════
// Trace infrastructure
// ═══════════════════════════════════════════════════════════════



// Thread-local sequence counter for ordering
static thread_local uint64_t t_trace_seq = 0;

// Thread-local TX ID counter for richer trace events
static thread_local uint64_t t_current_tx_id = 0;
static thread_local uint64_t t_tx_reads = 0;
static thread_local uint64_t t_tx_writes = 0;

// Helper: write a text-format trace line to a FILE*
static FILE *g_trace_file = nullptr;
static std::atomic<uint64_t> g_trace_ts{0};

// ── Trace helper: write a formatted line (extended format) ────────────
// Format: ts tid type txid addr width val cont_flag [extra...]
// Types: 0=read, 1=write, 2=tx_begin, 3=tx_end, 4=malloc, 5=free, 6=abort
static void trace_write_line_ext(FILE *f, uint64_t ts, uint64_t tid, int type,
                                 uint64_t txid, uint64_t addr, int width,
                                 uint64_t val, uint8_t cont_flag,
                                 const char *extra) {
    fprintf(f, "%llu %llu %d %llu 0x%llx %d 0x%llx %u%s%s\n",
            (unsigned long long)ts, (unsigned long long)tid, type,
            (unsigned long long)txid,
            (unsigned long long)addr, width, (unsigned long long)val,
            (unsigned)cont_flag,
            extra ? " " : "", extra ? extra : "");
}

// Legacy format writer (backward compat)
static void trace_write_line(FILE *f, uint64_t ts, uint64_t tid, int type,
                             uint64_t addr, int width, uint64_t val) {
    trace_write_line_ext(f, ts, tid, type, 0, addr, width, val, 0, nullptr);
}

// Individual trace wrapper functions (no templates to avoid type mismatch)
extern "C" {

static void trace_begin() {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_current_tx_id++;
        t_tx_reads = 0;
        t_tx_writes = 0;
        trace_write_line_ext(g_trace_file, ts, tid, 2, t_current_tx_id,
                             0, 0, 0, 0, nullptr);
    }
    if (s_trace_real_hooks.begin) s_trace_real_hooks.begin();
}

static void trace_end() {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line_ext(g_trace_file, ts, tid, 3, t_current_tx_id,
                             0, 0, (t_tx_reads << 32) | t_tx_writes, 0, nullptr);
    }
    if (s_trace_real_hooks.end) s_trace_real_hooks.end();
}

static void *trace_malloc(size_t s) {
    void *p = s_trace_real_hooks.malloc ? s_trace_real_hooks.malloc(s) : std::malloc(s);
    if (g_trace_file && p) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line_ext(g_trace_file, ts, tid, 4, t_current_tx_id,
                             (uint64_t)(uintptr_t)p, (int)s, 0, 0, nullptr);
    }
    return p;
}

static void *trace_calloc(size_t n, size_t s) {
    void *p = s_trace_real_hooks.calloc ? s_trace_real_hooks.calloc(n, s) : std::calloc(n, s);
    if (g_trace_file && p) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line_ext(g_trace_file, ts, tid, 4, t_current_tx_id,
                             (uint64_t)(uintptr_t)p, (int)(n * s), 0, 0, nullptr);
    }
    return p;
}

static void *trace_realloc(void *old, size_t s) {
    void *p = s_trace_real_hooks.realloc ? s_trace_real_hooks.realloc(old, s) : std::realloc(old, s);
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        if (old && p != old) {
            trace_write_line_ext(g_trace_file, ts, tid, 5, t_current_tx_id,
                                 (uint64_t)(uintptr_t)old, 0, 0, 0, nullptr);
        }
        if (p) {
            ts = g_trace_ts.fetch_add(1);
            trace_write_line_ext(g_trace_file, ts, tid, 4, t_current_tx_id,
                                 (uint64_t)(uintptr_t)p, (int)s, 0, 0, nullptr);
        }
    }
    return p;
}

static void trace_free(void *p) {
    if (g_trace_file && p) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line_ext(g_trace_file, ts, tid, 5, t_current_tx_id,
                             (uint64_t)(uintptr_t)p, 0, 0, 0, nullptr);
    }
    if (s_trace_real_hooks.free) s_trace_real_hooks.free(p);
    else if (p) std::free(p);
}

static uint8_t trace_read_i1(uint8_t *a) {
    uint8_t r = s_trace_real_hooks.read_i1 ? s_trace_real_hooks.read_i1(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 1, r, 0, nullptr);
    }
    return r;
}
static uint16_t trace_read_i2(uint16_t *a) {
    uint16_t r = s_trace_real_hooks.read_i2 ? s_trace_real_hooks.read_i2(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 2, r, 0, nullptr);
    }
    return r;
}
static uint32_t trace_read_i4(uint32_t *a) {
    uint32_t r = s_trace_real_hooks.read_i4 ? s_trace_real_hooks.read_i4(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 4, r, 0, nullptr);
    }
    return r;
}
static uint64_t trace_read_i8(uint64_t *a) {
    uint64_t r = s_trace_real_hooks.read_i8 ? s_trace_real_hooks.read_i8(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 8, r, 0, nullptr);
    }
    return r;
}
static float trace_read_f4(float *a) {
    float r = s_trace_real_hooks.read_f4 ? s_trace_real_hooks.read_f4(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        uint32_t tmp; memcpy(&tmp, &r, 4);
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 4, tmp, 0, nullptr);
    }
    return r;
}
static double trace_read_f8(double *a) {
    double r = s_trace_real_hooks.read_f8 ? s_trace_real_hooks.read_f8(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        uint64_t tmp; memcpy(&tmp, &r, 8);
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 8, tmp, 0, nullptr);
    }
    return r;
}
static void *trace_read_ptr(void **a) {
    void *r = s_trace_real_hooks.read_ptr ? s_trace_real_hooks.read_ptr(a) : *a;
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_reads++;
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 8, (uint64_t)(uintptr_t)r, 0, nullptr);
    }
    return r;
}

static void trace_write_i1(uint8_t *a, uint8_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 1, v, 0, nullptr);
    }
    if (s_trace_real_hooks.write_i1) s_trace_real_hooks.write_i1(a, v);
}
static void trace_write_i2(uint16_t *a, uint16_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 2, v, 0, nullptr);
    }
    if (s_trace_real_hooks.write_i2) s_trace_real_hooks.write_i2(a, v);
}
static void trace_write_i4(uint32_t *a, uint32_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 4, v, 0, nullptr);
    }
    if (s_trace_real_hooks.write_i4) s_trace_real_hooks.write_i4(a, v);
}
static void trace_write_i8(uint64_t *a, int64_t v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 8, (uint64_t)v, 0, nullptr);
    }
    if (s_trace_real_hooks.write_i8) s_trace_real_hooks.write_i8(a, v);
}
static void trace_write_f4(float *a, float v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        uint32_t tmp; memcpy(&tmp, &v, 4);
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 4, tmp, 0, nullptr);
    }
    if (s_trace_real_hooks.write_f4) s_trace_real_hooks.write_f4(a, v);
}
static void trace_write_f8(double *a, double v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        uint64_t tmp; memcpy(&tmp, &v, 8);
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 8, tmp, 0, nullptr);
    }
    if (s_trace_real_hooks.write_f8) s_trace_real_hooks.write_f8(a, v);
}
static void trace_write_ptr(void **a, void *v) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        t_tx_writes++;
        trace_write_line_ext(g_trace_file, ts, tid, 1, t_current_tx_id,
                             (uint64_t)(uintptr_t)a, 8, (uint64_t)(uintptr_t)v, 0, nullptr);
    }
    if (s_trace_real_hooks.write_ptr) s_trace_real_hooks.write_ptr(a, v);
}

} // extern "C"

// Trace abort event (called by backend on TX abort)
extern "C" void tm_trace_abort(uint64_t reason) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line_ext(g_trace_file, ts, tid, 6, t_current_tx_id,
                             0, 0, reason, 0, nullptr);
    }
}

// Record a contention event on a read/write
extern "C" void tm_trace_contention(uint64_t addr, int width, uint64_t val) {
    if (g_trace_file) {
        uint64_t ts = g_trace_ts.fetch_add(1);
        uint64_t tid = (uint64_t)(uintptr_t)pthread_self() & 0xffff;
        trace_write_line_ext(g_trace_file, ts, tid, 0, t_current_tx_id,
                             addr, width, val, 1, nullptr);
    }
}

// ═══════════════════════════════════════════════════════════════
// Invariant oracle (Phase 4 of fuzz tool plan)
// ═══════════════════════════════════════════════════════════════

static tm_invariant_callback_t g_invariant_cb = nullptr;
static void *g_invariant_data = nullptr;

// Saved original tm_end before we wrapped it
static void (*g_original_tm_end)() = nullptr;

extern "C" {

static void end_with_invariant() {
    if (g_original_tm_end) g_original_tm_end();
    if (g_invariant_cb) {
        int result = g_invariant_cb(g_invariant_data);
        if (result != 0) {
            fprintf(stderr, "TM-INVARIANT: FAIL (callback returned %d "
                            "after tx %llu on thread 0x%llx)\n",
                    result,
                    (unsigned long long)t_current_tx_id,
                    (unsigned long long)(uintptr_t)pthread_self());
        }
    }
}

} // extern "C"

extern "C" void tm_register_invariant_callback(tm_invariant_callback_t cb, void *data) {
    g_invariant_cb = cb;
    g_invariant_data = data;

    // Wrap tm_end to call the oracle after commit
    if (!g_original_tm_end) {
        g_original_tm_end = tm_end;
        tm_end = end_with_invariant;
    }
}

// Install trace wrappers when TM_TRACE_PATH env var is set.
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

    g_trace_active = true;

    // Save current hooks as trace real hooks (separate from s_real_hooks
    // so trace and sampling can coexist without recursion).
    s_trace_real_hooks.begin   = tm_begin;
    s_trace_real_hooks.end     = tm_end;
    s_trace_real_hooks.malloc  = tm_malloc;
    s_trace_real_hooks.calloc  = tm_calloc;
    s_trace_real_hooks.realloc = tm_realloc;
    s_trace_real_hooks.free    = tm_free;
    s_trace_real_hooks.read_i1 = tm_read_i1;
    s_trace_real_hooks.read_i2 = tm_read_i2;
    s_trace_real_hooks.read_i4 = tm_read_i4;
    s_trace_real_hooks.read_i8 = tm_read_i8;
    s_trace_real_hooks.read_f4 = tm_read_f4;
    s_trace_real_hooks.read_f8 = tm_read_f8;
    s_trace_real_hooks.read_ptr = tm_read_ptr;
    s_trace_real_hooks.write_i1 = tm_write_i1;
    s_trace_real_hooks.write_i2 = tm_write_i2;
    s_trace_real_hooks.write_i4 = tm_write_i4;
    s_trace_real_hooks.write_i8 = tm_write_i8;
    s_trace_real_hooks.write_f4 = tm_write_f4;
    s_trace_real_hooks.write_f8 = tm_write_f8;
    s_trace_real_hooks.write_ptr = tm_write_ptr;
    s_trace_real_hooks.get_env = tm_get_env;
    s_trace_real_hooks.set_jmpbuf = tm_set_jmpbuf;
    s_trace_real_hooks.get_thread_state = tm_get_thread_state;

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

// ═══════════════════════════════════════════════════════════════
// Sampling infrastructure (Phase 2 of fuzz tool plan)
// ═══════════════════════════════════════════════════════════════

// Sampling mode
enum class TMSamplingMode : uint8_t {
    None = 0,
    RateLimited,
    Phase,
    Adaptive
};

// Sampling config
static TMSamplingMode g_sampling_mode = TMSamplingMode::None;
static thread_local uint64_t t_sample_count = 0;
static thread_local uint64_t t_tx_count = 0;
static thread_local uint64_t t_abort_count = 0;
static uint64_t g_sample_rate = 1;        // 1 = every access, 10 = every 10th
static uint64_t g_phase_full = 10;        // first N transactions get full instrumentation
static thread_local bool t_phase_full_active = true;

// Sample rate getter/setter
extern "C" void tm_set_sample_rate(uint64_t rate) {
    g_sample_rate = rate > 0 ? rate : 1;
}

extern "C" uint64_t tm_get_sample_rate() {
    return g_sample_rate;
}

extern "C" void tm_set_sampling_mode(const char *mode) {
    if (std::strcmp(mode, "rate") == 0)
        g_sampling_mode = TMSamplingMode::RateLimited;
    else if (std::strcmp(mode, "phase") == 0)
        g_sampling_mode = TMSamplingMode::Phase;
    else if (std::strcmp(mode, "adaptive") == 0)
        g_sampling_mode = TMSamplingMode::Adaptive;
    else if (std::strcmp(mode, "none") == 0)
        g_sampling_mode = TMSamplingMode::None;
}

static bool should_sample() {
    switch (g_sampling_mode) {
    case TMSamplingMode::None:
        return true;
    case TMSamplingMode::RateLimited:
        return (++t_sample_count % g_sample_rate == 0);
    case TMSamplingMode::Phase:
        if (t_phase_full_active)
            return true;
        return (++t_sample_count % g_sample_rate == 0);
    case TMSamplingMode::Adaptive: {
        uint64_t rate = g_sample_rate;
        if (t_abort_count > 100)
            rate = g_sample_rate * 2;
        if (t_abort_count > 1000)
            rate = g_sample_rate * 4;
        return (++t_sample_count % rate == 0);
    }
    }
    return false;
}

extern "C" {

// Sampling wrappers — forward to real hooks based on sampling decision
static void sample_begin() {
    if (g_sampling_mode == TMSamplingMode::Phase) {
        if (t_tx_count >= g_phase_full)
            t_phase_full_active = false;
        t_tx_count++;
    }
    if (should_sample() && s_real_hooks.begin)
        s_real_hooks.begin();
}

static void sample_end() {
    if (should_sample() && s_real_hooks.end)
        s_real_hooks.end();
}

static void *sample_malloc(size_t s) {
    if (should_sample() && s_real_hooks.malloc)
        return s_real_hooks.malloc(s);
    return s_real_hooks.malloc ? s_real_hooks.malloc(s) : std::malloc(s);
}

static void *sample_calloc(size_t n, size_t s) {
    if (should_sample() && s_real_hooks.calloc)
        return s_real_hooks.calloc(n, s);
    return s_real_hooks.calloc ? s_real_hooks.calloc(n, s) : std::calloc(n, s);
}

static void *sample_realloc(void *old, size_t s) {
    if (should_sample() && s_real_hooks.realloc)
        return s_real_hooks.realloc(old, s);
    return s_real_hooks.realloc ? s_real_hooks.realloc(old, s) : std::realloc(old, s);
}

static void sample_free(void *p) {
    if (s_real_hooks.free) s_real_hooks.free(p);
    else if (p) std::free(p);
}

static uint8_t sample_read_i1(uint8_t *a) {
    return should_sample() && s_real_hooks.read_i1 ? s_real_hooks.read_i1(a) : *a;
}
static uint16_t sample_read_i2(uint16_t *a) {
    return should_sample() && s_real_hooks.read_i2 ? s_real_hooks.read_i2(a) : *a;
}
static uint32_t sample_read_i4(uint32_t *a) {
    return should_sample() && s_real_hooks.read_i4 ? s_real_hooks.read_i4(a) : *a;
}
static uint64_t sample_read_i8(uint64_t *a) {
    return should_sample() && s_real_hooks.read_i8 ? s_real_hooks.read_i8(a) : *a;
}
static float sample_read_f4(float *a) {
    return should_sample() && s_real_hooks.read_f4 ? s_real_hooks.read_f4(a) : *a;
}
static double sample_read_f8(double *a) {
    return should_sample() && s_real_hooks.read_f8 ? s_real_hooks.read_f8(a) : *a;
}
static void *sample_read_ptr(void **a) {
    return should_sample() && s_real_hooks.read_ptr ? s_real_hooks.read_ptr(a) : *a;
}

static void sample_write_i1(uint8_t *a, uint8_t v) {
    if (should_sample() && s_real_hooks.write_i1) s_real_hooks.write_i1(a, v);
    else *a = v;
}
static void sample_write_i2(uint16_t *a, uint16_t v) {
    if (should_sample() && s_real_hooks.write_i2) s_real_hooks.write_i2(a, v);
    else *a = v;
}
static void sample_write_i4(uint32_t *a, uint32_t v) {
    if (should_sample() && s_real_hooks.write_i4) s_real_hooks.write_i4(a, v);
    else *a = v;
}
static void sample_write_i8(uint64_t *a, int64_t v) {
    if (should_sample() && s_real_hooks.write_i8) s_real_hooks.write_i8(a, v);
    else *a = (uint64_t)v;
}
static void sample_write_f4(float *a, float v) {
    if (should_sample() && s_real_hooks.write_f4) s_real_hooks.write_f4(a, v);
    else *a = v;
}
static void sample_write_f8(double *a, double v) {
    if (should_sample() && s_real_hooks.write_f8) s_real_hooks.write_f8(a, v);
    else *a = v;
}
static void sample_write_ptr(void **a, void *v) {
    if (should_sample() && s_real_hooks.write_ptr) s_real_hooks.write_ptr(a, v);
    else *a = v;
}

// Abort notification for adaptive sampling
static void sample_notify_abort() {
    t_abort_count++;
}

} // extern "C"

// Install sampling wrappers when TM_SAMPLE_MODE env var is set.
// Called automatically at startup via constructor attribute.
__attribute__((constructor)) static void tm_sample_hook_init() {
    const char *mode = getenv("TM_SAMPLE_MODE");
    if (!mode || !mode[0])
        return;

    // Save current hooks as real hooks
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
    s_real_hooks.get_env = tm_get_env;
    s_real_hooks.set_jmpbuf = tm_set_jmpbuf;
    s_real_hooks.get_thread_state = tm_get_thread_state;

    // Set sampling mode
    tm_set_sampling_mode(mode);

    // Read sample rate from env
    const char *rate_str = getenv("TM_SAMPLE_RATE");
    if (rate_str && rate_str[0])
        tm_set_sample_rate(std::atoll(rate_str));

    const char *phase_full_str = getenv("TM_SAMPLE_PHASE_FULL");
    if (phase_full_str && phase_full_str[0])
        g_phase_full = std::atoll(phase_full_str);

    // Replace hooks with sampling wrappers
    tm_begin    = sample_begin;
    tm_end      = sample_end;
    tm_malloc   = sample_malloc;
    tm_calloc   = sample_calloc;
    tm_realloc  = sample_realloc;
    tm_free     = sample_free;
    tm_read_i1  = sample_read_i1;
    tm_read_i2  = sample_read_i2;
    tm_read_i4  = sample_read_i4;
    tm_read_i8  = sample_read_i8;
    tm_read_f4  = sample_read_f4;
    tm_read_f8  = sample_read_f8;
    tm_read_ptr = sample_read_ptr;
    tm_write_i1 = sample_write_i1;
    tm_write_i2 = sample_write_i2;
    tm_write_i4 = sample_write_i4;
    tm_write_i8 = sample_write_i8;
    tm_write_f4 = sample_write_f4;
    tm_write_f8 = sample_write_f8;
    tm_write_ptr= sample_write_ptr;
}

__attribute__((destructor)) static void tm_trace_hook_fini() {
    if (g_trace_file) {
        fflush(g_trace_file);
        if (g_trace_file != stderr && g_trace_file != stdout)
            fclose(g_trace_file);
        g_trace_file = nullptr;
    }
}
