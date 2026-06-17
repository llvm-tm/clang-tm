#pragma once

#include <cstddef>
#include <cstdint>

// ═══════════════════════════════════════════════════════════════════════
//  Stable Hook Interface — how to create a new TM backend runtime
// ═══════════════════════════════════════════════════════════════════════
//
// The TM system uses function-pointer variables (not forwarding functions)
// for ALL hook operations.  This enables:
//   1. Stub/real swapping: single‑thread → multi‑thread transparently
//   2. Phase‑based TM: swap backends at runtime via tm_swap_runtime()
//   3. Immediate effect: LLVM plugin emits indirect calls through these
//      pointers, so swaps take effect for instrumented code too.
//
// ── RULE: EVERY HOOK IS A DATA VARIABLE ──────────────────────────────
//
// All hooks declared below with "extern void (*NAME)()" are DATA variables
// — function pointers, NOT function declarations.  Backend runtime files
// MUST NOT define a function with the same name (e.g. "void tm_begin() { }"
// would collide with "void (*tm_begin)()").
//
// ── HOW TO CREATE A NEW BACKEND RUNTIME ──────────────────────────────
//
// In your backend_runtime.cpp:
//
//   #include "tm_hooks.hpp"
//
//   // 1. Implement each hook as a STATIC function
//   static void real_tm_begin() { ... }
//   static void real_tm_end()   { ... }
//   static uint8_t real_tm_read_i1(uint8_t *a) { ... }
//   // ... all 21 hooks (7 reads + 7 writes + 5 lifecycle + 2 misc)
//
//   // 2. Use RETURN TYPE "void*" for get_thread_state / get_env
//   static void *real_tm_get_thread_state() { return (void*)&my_state; }
//   static void *real_tm_get_env() { return (void*)&tm_jmpbuf; }
//   static void  real_tm_set_jmpbuf(void *buf) { }
//
//   // 3. Build the registration table with designated initializers
//   const TMRealHooks g_my_backend_hooks = {
//       .begin    = real_tm_begin,
//       .end      = real_tm_end,
//       .malloc   = real_tm_malloc,
//       .calloc   = real_tm_calloc,
//       .free     = real_tm_free,
//       .read_i1  = real_tm_read_i1,
//       // ... remaining read/write hooks ...
//       .write_ptr = real_tm_write_ptr,
//       .get_env   = real_tm_get_env,
//       .set_jmpbuf = real_tm_set_jmpbuf,
//       .get_thread_state = real_tm_get_thread_state,
//   };
//
//   // 4. Register in tm_init()
//   void tm_init() {
//       tm_register_real_hooks(&g_my_backend_hooks);
//   }
//
// ── WHAT NOT TO DO ───────────────────────────────────────────────────
//
//   ❌ extern "C" TMThreadState *tm_get_thread_state() { ... }
//      → Collides with "extern void *(*tm_get_thread_state)()" below.
//        Use "static void *real_tm_get_thread_state()" instead.
//
//   ❌ void tm_begin() { ... }
//      → Same collision.  Use "static void real_tm_begin() { }".
//
// ═══════════════════════════════════════════════════════════════════════

// ── Public hook variables ─────────────────────────────────────────
extern "C" {

// Lifecycle
extern void     (*tm_begin)();
extern void     (*tm_end)();
extern void    *(*tm_malloc)(size_t);
extern void    *(*tm_calloc)(size_t, size_t);
extern void    *(*tm_realloc)(void*, size_t);
extern void     (*tm_free)(void*);

// Reads
extern uint8_t  (*tm_read_i1)(uint8_t*);
extern uint16_t (*tm_read_i2)(uint16_t*);
extern uint32_t (*tm_read_i4)(uint32_t*);
extern uint64_t (*tm_read_i8)(uint64_t*);
extern float    (*tm_read_f4)(float*);
extern double   (*tm_read_f8)(double*);
extern void    *(*tm_read_ptr)(void**);

// Writes
extern void (*tm_write_i1)(uint8_t*, uint8_t);
extern void (*tm_write_i2)(uint16_t*, uint16_t);
extern void (*tm_write_i4)(uint32_t*, uint32_t);
extern void (*tm_write_i8)(uint64_t*, int64_t);
extern void (*tm_write_f4)(float*, float);
extern void (*tm_write_f8)(double*, double);
extern void (*tm_write_ptr)(void**, void*);

// Sigsetjmp hook (system sigsetjmp wrapper — always the same regardless of backend)
extern int (*tm_sigsetjmp)(void*, int);

// Trace hook (emitted when --emit-tm-trace is passed to the instrument pass)
extern void (*tm_trace)(uint32_t, void*, uint64_t, uint64_t);

// Thread-state hook (each backend provides its own implementation)
extern void *(*tm_get_thread_state)();

} // extern "C"

// ── Registration API ────────────────────────────────────────
// Each backend populates a TMRealHooks struct and calls
// tm_register_real_hooks() in tm_init(). The hooks are applied
// immediately based on current thread count.

struct TMRealHooks {
    void     (*begin)()                    = nullptr;
    void     (*end)()                      = nullptr;
    void    *(*malloc)(size_t)             = nullptr;
    void    *(*calloc)(size_t, size_t)     = nullptr;
    void    *(*realloc)(void*, size_t)     = nullptr;
    void     (*free)(void*)                = nullptr;
    uint8_t  (*read_i1)(uint8_t*)          = nullptr;
    uint16_t (*read_i2)(uint16_t*)         = nullptr;
    uint32_t (*read_i4)(uint32_t*)         = nullptr;
    uint64_t (*read_i8)(uint64_t*)         = nullptr;
    float    (*read_f4)(float*)            = nullptr;
    double   (*read_f8)(double*)           = nullptr;
    void    *(*read_ptr)(void**)           = nullptr;
    void     (*write_i1)(uint8_t*, uint8_t)   = nullptr;
    void     (*write_i2)(uint16_t*, uint16_t) = nullptr;
    void     (*write_i4)(uint32_t*, uint32_t) = nullptr;
    void     (*write_i8)(uint64_t*, int64_t)  = nullptr;
    void     (*write_f4)(float*, float)        = nullptr;
    void     (*write_f8)(double*, double)      = nullptr;
    void     (*write_ptr)(void**, void*)        = nullptr;
    void    *(*get_env)()                        = nullptr;
    void     (*set_jmpbuf)(void*)                = nullptr;
    void    *(*get_thread_state)()               = nullptr;
};

void tm_register_real_hooks(const TMRealHooks *hooks);
void tm_set_num_threads(int n);
void tm_hook_init_thread();
void tm_hook_exit_thread();

// Returns the currently registered real hooks (for use with tm_swap_runtime).
const TMRealHooks *tm_get_real_hooks();

// Phase-based TM: atomically swap ALL hooks under a global lock.
void tm_swap_runtime(const TMRealHooks *hooks);

// Sampling API (Phase 2 of fuzz tool plan)
extern "C" {
void tm_set_sample_rate(uint64_t rate);
uint64_t tm_get_sample_rate();
void tm_set_sampling_mode(const char *mode); // "none", "rate", "phase", "adaptive"
}

// Trace API (Phase 3 of fuzz tool plan)
extern "C" {
void tm_trace_abort(uint64_t reason);
void tm_trace_contention(uint64_t addr, int width, uint64_t val);
}

// Invariant oracle API (Phase 4 of fuzz tool plan)
typedef int (*tm_invariant_callback_t)(void *data);
extern "C" {
void tm_register_invariant_callback(tm_invariant_callback_t cb, void *data);
}
