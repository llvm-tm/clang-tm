#include <csetjmp>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

// ── Thread-local variables (same linkage as tm_api.hpp expects) ──
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
__thread sigjmp_buf tm_jmpbuf;

// ── Lifecycle — all no-ops ──────────────────────────────────────
extern "C" void tm_init() {}
extern "C" void tm_exit() {}
extern "C" void tm_init_thread() {}
extern "C" void tm_exit_thread() {}

// ── Transaction begin/end — no-ops (single-threaded baseline) ───
extern "C" void tm_begin() {}
extern "C" void tm_end() {}

// ── Allocation — pass through to system allocator ────────────────
extern "C" void *tm_malloc(size_t size)       { return std::malloc(size); }
extern "C" void *tm_calloc(size_t nmemb, size_t size) { return std::calloc(nmemb, size); }
extern "C" void *tm_realloc(void *ptr, size_t size)  { return std::realloc(ptr, size); }
extern "C" void  tm_free(void *ptr)           { std::free(ptr); }

// ── TM read/write — direct memory access (no transactional logic) ──
extern "C" uint8_t  tm_read_i1(uint8_t *addr)  { return *addr; }
extern "C" uint16_t tm_read_i2(uint16_t *addr) { return *addr; }
extern "C" uint32_t tm_read_i4(uint32_t *addr) { return *addr; }
extern "C" uint64_t tm_read_i8(uint64_t *addr) { return *addr; }
extern "C" float    tm_read_f4(float *addr)     { return *addr; }
extern "C" double   tm_read_f8(double *addr)    { return *addr; }
extern "C" void    *tm_read_ptr(void **addr)    { return *addr; }

extern "C" void tm_write_i1(uint8_t *addr, uint8_t val)   { *addr = val; }
extern "C" void tm_write_i2(uint16_t *addr, uint16_t val) { *addr = val; }
extern "C" void tm_write_i4(uint32_t *addr, uint32_t val) { *addr = val; }
extern "C" void tm_write_i8(uint64_t *addr, int64_t val)  { *addr = val; }
extern "C" void tm_write_f4(float *addr, float val)       { *addr = val; }
extern "C" void tm_write_f8(double *addr, double val)     { *addr = val; }
extern "C" void tm_write_ptr(void **addr, void *val)      { *addr = val; }
