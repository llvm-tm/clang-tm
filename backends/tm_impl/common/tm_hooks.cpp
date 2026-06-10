#include "tm_hooks.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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
// Hook pointer definitions — default to stubs
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
}

// ═══════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════

static std::atomic<int> s_num_threads{0};
static TMRealHooks s_real_hooks;
static bool s_registered = false;

static void apply_hooks() {
    // Default (0) = real hooks (backward compatible).
    // Explicit tm_set_num_threads(1) = stubs (single-thread bypass).
    // tm_set_num_threads(N>1) = real hooks.
    bool single = s_num_threads.load() == 1;
    const TMRealHooks *r = &s_real_hooks;

    tm_begin    = single ? stub_begin    : (r->begin    ? r->begin    : stub_begin);
    tm_end      = single ? stub_end      : (r->end      ? r->end      : stub_end);
    tm_malloc   = single ? stub_malloc   : (r->malloc   ? r->malloc   : stub_malloc);
    tm_calloc   = single ? stub_calloc   : (r->calloc   ? r->calloc   : stub_calloc);
    tm_realloc  = single ? stub_realloc  : (r->realloc  ? r->realloc  : stub_realloc);
    tm_free     = single ? stub_free     : (r->free     ? r->free     : stub_free);
    tm_read_i1  = single ? stub_read_i1  : (r->read_i1  ? r->read_i1  : stub_read_i1);
    tm_read_i2  = single ? stub_read_i2  : (r->read_i2  ? r->read_i2  : stub_read_i2);
    tm_read_i4  = single ? stub_read_i4  : (r->read_i4  ? r->read_i4  : stub_read_i4);
    tm_read_i8  = single ? stub_read_i8  : (r->read_i8  ? r->read_i8  : stub_read_i8);
    tm_read_f4  = single ? stub_read_f4  : (r->read_f4  ? r->read_f4  : stub_read_f4);
    tm_read_f8  = single ? stub_read_f8  : (r->read_f8  ? r->read_f8  : stub_read_f8);
    tm_read_ptr = single ? stub_read_ptr : (r->read_ptr ? r->read_ptr : stub_read_ptr);
    tm_write_i1 = single ? stub_write_i1 : (r->write_i1 ? r->write_i1 : stub_write_i1);
    tm_write_i2 = single ? stub_write_i2 : (r->write_i2 ? r->write_i2 : stub_write_i2);
    tm_write_i4 = single ? stub_write_i4 : (r->write_i4 ? r->write_i4 : stub_write_i4);
    tm_write_i8 = single ? stub_write_i8 : (r->write_i8 ? r->write_i8 : stub_write_i8);
    tm_write_f4 = single ? stub_write_f4 : (r->write_f4 ? r->write_f4 : stub_write_f4);
    tm_write_f8 = single ? stub_write_f8 : (r->write_f8 ? r->write_f8 : stub_write_f8);
    tm_write_ptr= single ? stub_write_ptr: (r->write_ptr? r->write_ptr: stub_write_ptr);
}

void tm_register_real_hooks(const TMRealHooks *hooks) {
    s_real_hooks = *hooks;
    s_registered = true;
    apply_hooks();
}

void tm_set_num_threads(int n) {
    s_num_threads.store(n);
    if (s_registered)
        apply_hooks();
}
