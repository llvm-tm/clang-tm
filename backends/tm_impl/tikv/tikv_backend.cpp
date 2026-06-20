// ── TiKV Backend — C++ hook shim ────────────────────────
//
// This file bridges the C++ TM hook system to the Rust tikv-
// client crate.  The Rust crate (runtime/tikv/) is compiled as
// both a Rust library (libruntime_tikv.rlib) and a C-compatible
// static library (libruntime_tikv.a).  This .cpp declares those
// functions as extern "C" and wraps them in static hook functions
// that are registered via TMRealHooks.
//
// To build:
//   1. cd backends/tm_impl/tikv && cargo build --release
//   2. cd benchmarks/cpp && make BACKEND=TIKV
//
// Build integration (benchmarks/cpp/Makefile):
//   ifeq ($(BACKEND),TIKV)
//     RUST_TIKV = ../../backends/tm_impl/tikv
//     RUNTIME   = $(RUST_TIKV)/tikv_backend.cpp
//     DEFS      = -DTM_BACKEND_TIKV
//     EXTRA_INC = -I../../backends/tm_impl/common
//     LDFLAGS  += $(RUST_TIKV)/target/release/libruntime_tikv.a \
//                 -lpthread -ldl -lm
//     # cargo build must run before make
//     $(RUST_TIKV)/target/release/libruntime_tikv.a: FORCE
//             cd $(RUST_TIKV) && cargo build --release
//     $(BENCH): $(RUST_TIKV)/target/release/libruntime_tikv.a
//   endif
//
// NOTE: The Rust tokio runtime + tikv-client must be linked
// into the final binary.  To avoid symbol conflicts, the Rust
// crate uses #[no_mangle] for all exported symbols.

extern "C" {
    // Rust FFI symbols from runtime-tikv
    void tikv_tm_init();
    void tikv_tm_exit();
    void tikv_tm_begin();
    bool tikv_tm_end();
    uint8_t  tikv_tm_read_u8(const uint8_t*);
    uint16_t tikv_tm_read_u16(const uint16_t*);
    uint32_t tikv_tm_read_u32(const uint32_t*);
    uint64_t tikv_tm_read_u64(const uint64_t*);
    int8_t   tikv_tm_read_i8(const int8_t*);
    int16_t  tikv_tm_read_i16(const int16_t*);
    int32_t  tikv_tm_read_i32(const int32_t*);
    int64_t  tikv_tm_read_i64(const int64_t*);
    float    tikv_tm_read_f32(const float*);
    double   tikv_tm_read_f64(const double*);
    void*    tikv_tm_read_ptr(void* const*);
    void tikv_tm_write_u8(uint8_t*, uint8_t);
    void tikv_tm_write_u16(uint16_t*, uint16_t);
    void tikv_tm_write_u32(uint32_t*, uint32_t);
    void tikv_tm_write_u64(uint64_t*, uint64_t);
    void tikv_tm_write_i8(int8_t*, int8_t);
    void tikv_tm_write_i16(int16_t*, int16_t);
    void tikv_tm_write_i32(int32_t*, int32_t);
    void tikv_tm_write_i64(int64_t*, int64_t);
    void tikv_tm_write_f32(float*, float);
    void tikv_tm_write_f64(double*, double);
    void tikv_tm_write_ptr(void**, void*);
    void* tikv_tm_get_thread_state();
}

#include "tm_hooks.hpp"
#include "tm_thread_state.hpp"
#include "tm_alloc_overrides.hpp"

// ── Static hook implementations ──────────────────────────

static void real_tm_begin()       { tikv_tm_begin(); }
static void real_tm_end()         { tikv_tm_end(); }

static void* real_tm_malloc(size_t sz)   { return stm::tm_region_malloc(sz); }
static void* real_tm_calloc(size_t n, size_t sz) { return stm::tm_region_calloc(n, sz); }
static void* real_tm_realloc(void* p, size_t sz) { return stm::tm_region_realloc(p, sz); }
static void  real_tm_free(void* p)       { stm::tm_region_free(p); }

static uint8_t  real_tm_read_i1 (uint8_t  *a) { return tikv_tm_read_u8(a); }
static uint16_t real_tm_read_i2 (uint16_t *a) { return tikv_tm_read_u16(a); }
static uint32_t real_tm_read_i4 (uint32_t *a) { return tikv_tm_read_u32(a); }
static uint64_t real_tm_read_i8 (uint64_t *a) { return tikv_tm_read_u64(a); }
static float    real_tm_read_f4 (float    *a) { return tikv_tm_read_f32(a); }
static double   real_tm_read_f8 (double   *a) { return tikv_tm_read_f64(a); }
static void*    real_tm_read_p  (void*    *a) { return tikv_tm_read_ptr(a); }

static void real_tm_write_i1 (uint8_t  *a, uint8_t  v) { tikv_tm_write_u8(a, v); }
static void real_tm_write_i2 (uint16_t *a, uint16_t v) { tikv_tm_write_u16(a, v); }
static void real_tm_write_i4 (uint32_t *a, uint32_t v) { tikv_tm_write_u32(a, v); }
static void real_tm_write_i8 (uint64_t *a, uint64_t v) { tikv_tm_write_u64(a, v); }
static void real_tm_write_f4 (float    *a, float    v) { tikv_tm_write_f32(a, v); }
static void real_tm_write_f8 (double   *a, double   v) { tikv_tm_write_f64(a, v); }
static void real_tm_write_p  (void*    *a, void*    v) { tikv_tm_write_ptr(a, v); }

static void* real_tm_get_thread_state() { return tikv_tm_get_thread_state(); }

// ── Hook table ───────────────────────────────────────────

static constexpr TMRealHooks g_tikv_hooks = {
    .begin    = real_tm_begin,
    .end      = real_tm_end,
    .malloc   = real_tm_malloc,
    .calloc   = real_tm_calloc,
    .realloc  = real_tm_realloc,
    .free     = real_tm_free,
    .read_i1  = real_tm_read_i1,
    .read_i2  = real_tm_read_i2,
    .read_i4  = real_tm_read_i4,
    .read_i8  = real_tm_read_i8,
    .read_f4  = real_tm_read_f4,
    .read_f8  = real_tm_read_f8,
    .read_p   = real_tm_read_p,
    .write_i1 = real_tm_write_i1,
    .write_i2 = real_tm_write_i2,
    .write_i4 = real_tm_write_i4,
    .write_i8 = real_tm_write_i8,
    .write_f4 = real_tm_write_f4,
    .write_f8 = real_tm_write_f8,
    .write_p  = real_tm_write_p,
    .get_thread_state = real_tm_get_thread_state,
    .set_jmpbuf = nullptr,
    .get_env    = nullptr,
};

// ── LLVM_TM_PLUGIN wrappers ──────────────────────────────
// The plugin expects tm_init/tm_exit etc. as DATA variables
// (function pointers), not bare TEXT functions.

#ifdef LLVM_TM_PLUGIN

static void do_tm_init();
static void do_tm_exit();
static void do_tm_init_thread();
static void do_tm_exit_thread();

void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;

static void do_tm_init()
#else
void tm_init()
#endif
{
    stm::tm_region_init();
    tikv_tm_init();
    tm_register_real_hooks(&g_tikv_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    tikv_tm_exit();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
    tm_hook_init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{
    tm_hook_exit_thread();
}
