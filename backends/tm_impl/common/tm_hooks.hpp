#pragma once

#include <cstddef>
#include <cstdint>

// ── Public hook variables ─────────────────────────────────────────
// These are the actual extern "C" symbols seen by all callers (both
// plugin-generated code and explicit API).
//
// Each is a function-pointer variable (not a forwarding function) so
// that the runtime can swap the hook at runtime for:
//   - single-thread stubs (direct access, no TM overhead)
//   - multi-thread real TM implementations
//   - phase-based TM (different backends based on abort rate)
//
// The LLVM plugin emits indirect calls (load + call through) so that
// hook swaps take effect immediately for instrumented code too.

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
