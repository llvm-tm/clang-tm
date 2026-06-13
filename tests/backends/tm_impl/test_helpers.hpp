#pragma once

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// ── Runtime stub declarations ──────────────────────────────────────
// Every backend runtime provides these symbols.
// Parameters marked (unused) are ignored by some backends but present
// for ABI compatibility across all runtimes.

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;

void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();

// TX lifecycle — function-pointer variables from tm_hooks.cpp
extern void     (*tm_begin)();
extern void     (*tm_end)();
extern void    *(*tm_malloc)(size_t);
extern void    *(*tm_calloc)(size_t, size_t);
extern void    *(*tm_realloc)(void*, size_t);
extern void     (*tm_free)(void*);

// Read/write function-pointer variables
extern uint8_t  (*tm_read_i1)(uint8_t*);
extern void     (*tm_write_i1)(uint8_t*, uint8_t);
extern uint16_t (*tm_read_i2)(uint16_t*);
extern void     (*tm_write_i2)(uint16_t*, uint16_t);
extern uint32_t (*tm_read_i4)(uint32_t*);
extern void     (*tm_write_i4)(uint32_t*, uint32_t);
extern uint64_t (*tm_read_i8)(uint64_t*);
extern void     (*tm_write_i8)(uint64_t*, int64_t);
extern float    (*tm_read_f4)(float*);
extern void     (*tm_write_f4)(float*, float);
extern double   (*tm_read_f8)(double*);
extern void     (*tm_write_f8)(double*, double);
extern void    *(*tm_read_ptr)(void**);
extern void     (*tm_write_ptr)(void**, void*);
}

// ── Convenience wrappers (omit symbol_id for test code) ────────────
inline uint8_t  tm_r1(uint8_t *a)  { return tm_read_i1(a); }
inline uint16_t tm_r2(uint16_t *a) { return tm_read_i2(a); }
inline uint32_t tm_r4(uint32_t *a) { return tm_read_i4(a); }
inline uint64_t tm_r8(uint64_t *a) { return tm_read_i8(a); }
inline void     tm_w1(uint8_t *a, uint8_t v)  { tm_write_i1(a, v); }
inline void     tm_w2(uint16_t *a, uint16_t v) { tm_write_i2(a, v); }
inline void     tm_w4(uint32_t *a, uint32_t v) { tm_write_i4(a, v); }
inline void     tm_w8(uint64_t *a, uint64_t v) { tm_write_i8(a, (int64_t)v); }

// ── Retry loop: works with both longjmp-based and flag-based backends ──
//
// For longjmp backends (TinySTM wbctl/wbetl):  sigsetjmp saves the
//   recovery point; abort_tx() does siglongjmp back here.  The
//   tm_longjmp_ret != 0 branch is taken on re-entry after abort.
//
// For flag-based backends (TL2, SwissTM, etc.): tm_begin/tm_end return
//   normally; the committed flag ensures exactly one attempt succeeds.
//   (If the backend never longjmps, the loop runs once.)
//
// Usage:
//   tm_nested_call_counter++;
//   tm_transaction([&]() {
//       uint32_t v = tm_read_i4(&shared_counter);
//       tm_write_i4(&shared_counter, v + 1);
//   });
//   tm_nested_call_counter--;

template <typename F>
inline void tm_transaction(F&& body) {
    int committed = 0;
    while (!committed) {
        tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
        tm_begin();
        if (tm_longjmp_ret != 0)
            continue;
        body();
        tm_end();
        committed = 1;
    }
}

// ── Result checking helpers ────────────────────────────────────────

struct TestResult {
    const char* name;
    bool ok;
    int64_t got;
    int64_t expected;
};

inline int check_result(const TestResult& r) {
    printf("  %s: ", r.name);
    if (r.ok) {
        printf("PASS (got %lld)\n", (long long)r.got);
        return 0;
    } else {
        printf("FAIL (got %lld, expected %lld)\n",
               (long long)r.got, (long long)r.expected);
        return 1;
    }
}
