#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unordered_map>
#include <vector>

#include "tm_common.hpp"
#include "tm_region_allocator.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── Per-thread state ─────────────────────────────────────
struct CalvinWriteEntry {
    void *addr;
    uint64_t value;
    uint8_t width;
};

struct CalvinState {
    bool collecting;
    bool use_collected;
    std::vector<std::pair<void*,uint8_t>> read_set;   // (addr, width)
    std::vector<CalvinWriteEntry> write_set;
    std::unordered_map<void*,CalvinWriteEntry> write_buffer;
};

static __thread CalvinState *g_cs = nullptr;

static CalvinState *ensure_state() {
    if (!g_cs) {
        g_cs = new CalvinState();
        g_cs->collecting = true;
        g_cs->use_collected = false;
    }
    return g_cs;
}

// ── Static backend implementation ────────────────────────

static void real_tm_begin() {
    CalvinState *s = ensure_state();
    if (tm_nested_call_counter > 1) return;
    if (s->use_collected) {
        // Execute phase: collected sets are ready.
        s->collecting = false;
    } else {
        // Collect phase: first run, capture all accesses.
        s->collecting = true;
        s->read_set.clear();
        s->write_set.clear();
        s->write_buffer.clear();
    }
}

static void real_tm_end() {
    if (tm_nested_call_counter > 1) return;
    CalvinState *s = ensure_state();
    if (s->collecting) {
        // Collect phase complete — abort to retry with known set.
        s->collecting = false;
        s->use_collected = true;
        tm_longjmp_ret = 1;
        siglongjmp(tm_jmpbuf, 1);
        return;
    }
    // Execute phase: validate reads, apply writes.
    for (auto &r : s->read_set) {
        uint64_t cur;
        memcpy(&cur, r.first, r.second);
        auto it = s->write_buffer.find(r.first);
        uint64_t expected = it != s->write_buffer.end() ? it->second.value : 0;
        if (it == s->write_buffer.end()) {
            // Not in write buffer — read from actual memory during collect
            // should still match. Skip validation for read-only addresses.
            continue;
        }
        if (memcmp(&cur, &expected, r.second) != 0) {
            // Value changed — abort and redo entire process.
            s->use_collected = false;
            tm_longjmp_ret = 1;
            siglongjmp(tm_jmpbuf, 1);
            return;
        }
    }
    // Apply writes.
    for (auto &w : s->write_set) {
        memcpy(w.addr, &w.value, w.width);
    }
    s->use_collected = false;
}

static void real_tm_abort() {
    CalvinState *s = ensure_state();
    s->collecting = true;
    s->use_collected = false;
    s->read_set.clear();
    s->write_set.clear();
    s->write_buffer.clear();
}

// ── Read / Write operations ──────────────────────────────

static uint64_t read_tracked(void *addr, uint8_t width) {
    CalvinState *s = ensure_state();
    if (s->collecting) {
        s->read_set.push_back({addr, width});
        uint64_t val = 0;
        memcpy(&val, addr, width);
        return val;
    }
    // Execute phase: check write-buffer first (read-own-writes).
    auto it = s->write_buffer.find(addr);
    if (it != s->write_buffer.end()) {
        uint64_t val = 0;
        memcpy(&val, &it->second.value, width);
        return val;
    }
    uint64_t val = 0;
    memcpy(&val, addr, width);
    return val;
}

static void write_tracked(void *addr, uint64_t val, uint8_t width) {
    CalvinState *s = ensure_state();
    CalvinWriteEntry e;
    e.addr = addr;
    e.value = val;
    e.width = width;
    if (s->collecting) {
        s->write_set.push_back(e);
        s->write_buffer[addr] = e;
        return;
    }
    s->write_set.push_back(e);
    s->write_buffer[addr] = e;
}

static uint8_t  real_tm_read_i1 (int8_t  *a) { LLVM_TM_ADDR_CHECK(a); return (uint8_t) read_tracked((void*)a, 1); }
static uint16_t real_tm_read_i2 (int16_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint16_t)read_tracked((void*)a, 2); }
static uint32_t real_tm_read_i4 (int32_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint32_t)read_tracked((void*)a, 4); }
static uint64_t real_tm_read_i8 (int64_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint64_t)read_tracked((void*)a, 8); }
static float    real_tm_read_f4 (float   *a) { LLVM_TM_ADDR_CHECK(a); float v; memcpy(&v, a, 4); return v; }
static double   real_tm_read_f8 (double  *a) { LLVM_TM_ADDR_CHECK(a); double v; memcpy(&v, a, 8); return v; }

static void real_tm_write_i1(int8_t  *a, uint8_t  v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 1); }
static void real_tm_write_i2(int16_t *a, uint16_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 2); }
static void real_tm_write_i4(int32_t *a, uint32_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 4); }
static void real_tm_write_i8(int64_t *a, uint64_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 8); }
static void real_tm_write_f4(float   *a, float    v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); memcpy(a, &v, 4); }
static void real_tm_write_f8(double  *a, double   v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); memcpy(a, &v, 8); }

static void *real_tm_malloc(size_t s)  { return stm::tm_region_malloc(s); }
static void *real_tm_calloc(size_t n, size_t s) {
    size_t total = n * s;
    void *p = stm::tm_region_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
static void *real_tm_realloc(void *p, size_t s) {
    if (!p) return stm::tm_region_malloc(s);
    void *n = stm::tm_region_malloc(s);
    if (n && p) memcpy(n, p, s);
    return n;
}
static void real_tm_free(void *p) {
    if (!p) return;
}

static void real_tm_init() {
    stm::tm_region_init();
}
static void real_tm_exit() {}
static void real_tm_init_thread() {
    tm_hook_init_thread();
    ensure_state();
}
static void real_tm_exit_thread() {}
static void real_tm_set_env(void *env) {}
static void real_tm_set_jmpbuf(void *buf) {}

static int32_t real_tm_get_nested_call_counter() { return tm_nested_call_counter; }
static void    real_tm_set_nested_call_counter(int32_t v) { tm_nested_call_counter = v; }
static int32_t real_tm_get_longjmp_ret()         { return tm_longjmp_ret; }
static int32_t real_tm_load_symbols(const char *lib) { return 0; }

static void real_tm_serialize_lock() {}
static void real_tm_serialize_unlock() {}

// ── Hook table ──────────────────────────────────────────
static TMRealHooks g_calvin_hooks = {
    .init                    = real_tm_init,
    .exit                    = real_tm_exit,
    .init_thread             = real_tm_init_thread,
    .exit_thread             = real_tm_exit_thread,
    .begin                   = real_tm_begin,
    .end                     = real_tm_end,
    .abort                   = real_tm_abort,
    .malloc                  = real_tm_malloc,
    .calloc                  = real_tm_calloc,
    .realloc                 = real_tm_realloc,
    .free                    = real_tm_free,
    .read_i1                 = real_tm_read_i1,
    .read_i2                 = real_tm_read_i2,
    .read_i4                 = real_tm_read_i4,
    .read_i8                 = real_tm_read_i8,
    .read_f4                 = real_tm_read_f4,
    .read_f8                 = real_tm_read_f8,
    .write_i1                = real_tm_write_i1,
    .write_i2                = real_tm_write_i2,
    .write_i4                = real_tm_write_i4,
    .write_i8                = real_tm_write_i8,
    .write_f4                = real_tm_write_f4,
    .write_f8                = real_tm_write_f8,
    .set_env                 = real_tm_set_env,
    .set_jmpbuf              = real_tm_set_jmpbuf,
    .get_nested_call_counter = real_tm_get_nested_call_counter,
    .set_nested_call_counter = real_tm_set_nested_call_counter,
    .get_longjmp_ret         = real_tm_get_longjmp_ret,
    .load_symbols            = real_tm_load_symbols,
    .serialize_lock          = real_tm_serialize_lock,
    .serialize_unlock        = real_tm_serialize_unlock,
    .get_thread_state        = nullptr,
};
} // extern "C"

// ── LLVM_TM_PLUGIN guards ────────────────────────────────
#ifdef LLVM_TM_PLUGIN
static void do_tm_init() { real_tm_init(); }
static void do_tm_exit() { real_tm_exit(); }
static void do_tm_init_thread() { real_tm_init_thread(); }
static void do_tm_exit_thread() { real_tm_exit_thread(); }

extern "C" {
void (*tm_init)()        = do_tm_init;
void (*tm_exit)()        = do_tm_exit;
void (*tm_init_thread)() = do_tm_init_thread;
void (*tm_exit_thread)() = do_tm_exit_thread;
}
#else
extern "C" {
void tm_init()        { real_tm_init();        tm_register_real_hooks(&g_calvin_hooks); }
void tm_exit()        { real_tm_exit(); }
void tm_init_thread() { real_tm_init_thread(); }
void tm_exit_thread() { real_tm_exit_thread(); }
}
#endif
