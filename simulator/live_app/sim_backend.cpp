// ── SimBackend: TM hook implementation that calls into Rust ──
// Each TM operation forwards to a C FFI function in the Rust
// simulator process, which runs conflict detection, clock
// advancement, and shadow memory management.
//
// The Rust binary exports sim_tm_read, sim_tm_write, etc. as
// extern "C" symbols.  The SimBackend .so is loaded with
// RTLD_GLOBAL so the application .so (loaded later) resolves
// its tm_read_i4 etc. DATA symbols to the definitions here.

#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── Rust FFI functions (provided by the Rust simulator binary) ─
extern "C" {
    uint64_t sim_tm_read(uint64_t addr, uint8_t width, uint32_t thread_id);
    void     sim_tm_write(uint64_t addr, uint8_t width, uint64_t val, uint32_t thread_id);
    void     sim_tm_begin(uint32_t thread_id);
    uint8_t  sim_tm_end(uint32_t thread_id);   // 1=commit ok, 0=abort
    void     sim_tm_abort(uint32_t thread_id);
    uint64_t sim_tm_malloc(uint64_t size, uint32_t thread_id);
    void     sim_tm_free(uint64_t addr, uint32_t thread_id);
    void     sim_tm_set_jmpbuf(void *buf, uint32_t thread_id);
    void    *sim_tm_get_env(uint32_t thread_id);
    void    *sim_tm_get_thread_state(uint32_t thread_id);
}

// ── TLS variables (must be before functions that reference them) ─
__thread int32_t    tm_nested_call_counter = 0;
__thread int32_t    tm_longjmp_ret = 0;
__thread sigjmp_buf tm_jmpbuf;

// ── Per-thread ID tracking ────────────────────────────────────
static __thread uint32_t g_thread_id = 0;

// ── Private SimBackend implementations (static, C linkage) ────
extern "C" {

static void sim_init() {}
static void sim_exit() {}
static void sim_init_thread() {
    static std::atomic<uint32_t> next_id{0};
    g_thread_id = next_id.fetch_add(1, std::memory_order_relaxed);
}
static void sim_exit_thread() {}
static void sim_begin() { sim_tm_begin(g_thread_id); }
static void sim_end() {
    uint8_t ok = sim_tm_end(g_thread_id);
    if (!ok) {
        // Commit failed — signal retry to the app.
        tm_longjmp_ret = 1;
    }
}
static void sim_abort() { sim_tm_abort(g_thread_id); }

static void *sim_malloc(size_t s) {
    return (void*)(uintptr_t)sim_tm_malloc((uint64_t)s, g_thread_id);
}
static void *sim_calloc(size_t n, size_t s) {
    size_t total = n * s;
    void *p = (void*)(uintptr_t)sim_tm_malloc((uint64_t)total, g_thread_id);
    if (p) memset(p, 0, total);
    return p;
}
static void *sim_realloc(void *p, size_t s) {
    (void)p;
    return (void*)(uintptr_t)sim_tm_malloc((uint64_t)s, g_thread_id);
}
static void  sim_free(void *p) {
    sim_tm_free((uint64_t)(uintptr_t)p, g_thread_id);
}

static uint8_t  sim_read_i1(const uint8_t  *a) { return (uint8_t )sim_tm_read((uint64_t)(uintptr_t)a, 1, g_thread_id); }
static uint16_t sim_read_i2(const uint16_t *a) { return (uint16_t)sim_tm_read((uint64_t)(uintptr_t)a, 2, g_thread_id); }
static uint32_t sim_read_i4(const uint32_t *a) { return (uint32_t)sim_tm_read((uint64_t)(uintptr_t)a, 4, g_thread_id); }
static uint64_t sim_read_i8(const uint64_t *a) { return           sim_tm_read((uint64_t)(uintptr_t)a, 8, g_thread_id); }
static float    sim_read_f4(const float    *a) { uint32_t r = (uint32_t)sim_tm_read((uint64_t)(uintptr_t)a, 4, g_thread_id); float v; memcpy(&v, &r, 4); return v; }
static double   sim_read_f8(const double   *a) { uint64_t r =           sim_tm_read((uint64_t)(uintptr_t)a, 8, g_thread_id); double v; memcpy(&v, &r, 8); return v; }
static void    *sim_read_ptr(void * const *a)  { return (void*)(uintptr_t)sim_tm_read((uint64_t)(uintptr_t)a, 8, g_thread_id); }

static void sim_write_i1(uint8_t  *a, uint8_t  v) { sim_tm_write((uint64_t)(uintptr_t)a, 1, (uint64_t)v, g_thread_id); }
static void sim_write_i2(uint16_t *a, uint16_t v) { sim_tm_write((uint64_t)(uintptr_t)a, 2, (uint64_t)v, g_thread_id); }
static void sim_write_i4(uint32_t *a, uint32_t v) { sim_tm_write((uint64_t)(uintptr_t)a, 4, (uint64_t)v, g_thread_id); }
static void sim_write_i8(uint64_t *a, int64_t  v) { sim_tm_write((uint64_t)(uintptr_t)a, 8, (uint64_t)v, g_thread_id); }
static void sim_write_f4(float    *a, float    v) { uint32_t r; memcpy(&r, &v, 4); sim_tm_write((uint64_t)(uintptr_t)a, 4, (uint64_t)r, g_thread_id); }
static void sim_write_f8(double   *a, double   v) { uint64_t r; memcpy(&r, &v, 8); sim_tm_write((uint64_t)(uintptr_t)a, 8, r, g_thread_id); }
static void sim_write_ptr(void   **a, void    *v) { sim_tm_write((uint64_t)(uintptr_t)a, 8, (uint64_t)(uintptr_t)v, g_thread_id); }

static void  sim_set_jmpbuf(void *buf) { sim_tm_set_jmpbuf(buf, g_thread_id); }
static void *sim_get_env()             { return sim_tm_get_env(g_thread_id); }
static void *sim_get_thread_state()    { return sim_tm_get_thread_state(g_thread_id); }

// ── sigsetjmp wrapper ─────────────────────────────────────────
// Must be a proper wrapping function, not just DATA-pointer to sigsetjmp,
// because the LLVM pass generates indirect calls through this pointer
// and expects it to behave like sigsetjmp (save/restore context).
// For the live-app simulator, we save the setjmp buf in sim_tm_set_jmpbuf.
// We don't actually need to save CPU registers — abort simulation just
// returns control via the normal retry path (lazy abort).
static int sim_sigsetjmp(void *env, int savemask) {
    // Save the jmpbuf pointer so sim_end can siglongjmp on abort.
    g_saved_jmpbuf = env;
    // Actually call sigsetjmp on the provided buffer so the execution
    // context is saved.  siglongjmp in sim_end will restore this.
    return sigsetjmp(*(sigjmp_buf*)env, savemask);
}

// Plain TEXT functions (tm_init/tm_exit called directly by app)
void tm_init()        { sim_init(); }
void tm_exit()        { sim_exit(); }
void tm_init_thread() { sim_init_thread(); }
void tm_exit_thread() { sim_exit_thread(); }

// ── Function-pointer DATA symbols ─────────────────────────────
void     (*tm_begin)()              = sim_begin;
void     (*tm_end)()                = sim_end;
void     (*tm_abort)()              = sim_abort;
void    *(*tm_malloc)(size_t)       = sim_malloc;
void    *(*tm_calloc)(size_t,size_t)= sim_calloc;
void    *(*tm_realloc)(void*,size_t)= sim_realloc;
void     (*tm_free)(void*)          = sim_free;

uint8_t  (*tm_read_i1)(const uint8_t*)   = sim_read_i1;
uint16_t (*tm_read_i2)(const uint16_t*)  = sim_read_i2;
uint32_t (*tm_read_i4)(const uint32_t*)  = sim_read_i4;
uint64_t (*tm_read_i8)(const uint64_t*)  = sim_read_i8;
float    (*tm_read_f4)(const float*)     = sim_read_f4;
double   (*tm_read_f8)(const double*)    = sim_read_f8;
void    *(*tm_read_ptr)(void* const*)    = sim_read_ptr;

void (*tm_write_i1)(uint8_t*, uint8_t)           = sim_write_i1;
void (*tm_write_i2)(uint16_t*, uint16_t)         = sim_write_i2;
void (*tm_write_i4)(uint32_t*, uint32_t)         = sim_write_i4;
void (*tm_write_i8)(uint64_t*, int64_t)          = sim_write_i8;
void (*tm_write_f4)(float*, float)               = sim_write_f4;
void (*tm_write_f8)(double*, double)             = sim_write_f8;
void (*tm_write_ptr)(void**, void*)              = sim_write_ptr;

void   (*tm_set_jmpbuf)(void*)          = sim_set_jmpbuf;
void  *(*tm_get_env)()                  = sim_get_env;
void  *(*tm_get_thread_state)()         = sim_get_thread_state;
int    (*tm_sigsetjmp)(void*, int)      = sim_sigsetjmp;

} // extern "C"
