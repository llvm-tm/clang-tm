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

#include "tm_hooks.hpp"
#include "tm_common.hpp"
#include "tm_region_allocator.hpp"

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── Calvin: Two-Phase OCC ───────────────────────────────
//
// Phase 1 (Collect):  Execute transaction body, buffer all writes
//   in a write-set, record read-set addresses AND captured values.
//   On commit, ABORT via siglongjmp.  The read/write set survives
//   in TLS.
//
// Phase 2 (Execute):  Re-execute transaction body.  Reads check
//   the write-buffer (read-own-writes).  Writes are buffered.
//   On commit, validate each read-set address against its captured
//   value.  If any changed, abort back to Phase 1.
//   Otherwise, apply write-set to memory.
//
// The two-phase approach is at a disadvantage vs normal OCC
// (executes the body twice).  Its advantage emerges with
// async/queue execution where the collect phase can be
// a lightweight speculation and the execute phase a fast,
// pre-validated deterministic replay.

struct CalvinWriteEntry {
    void   *addr;
    uint64_t value;
    uint8_t  width;
};

struct CalvinReadEntry {
    void   *addr;
    uint64_t captured;
    uint8_t  width;
};

struct CalvinState {
    bool collecting;
    bool use_collected;
    std::vector<CalvinReadEntry>             read_entries;
    std::vector<CalvinWriteEntry>            write_set;
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

static void real_tm_begin() {
    CalvinState *s = ensure_state();
    if (tm_nested_call_counter > 1) return;
    if (s->use_collected) {
        s->collecting = false;
        s->write_set.clear();
        s->write_buffer.clear();
    } else {
        s->collecting = true;
        s->read_entries.clear();
        s->write_set.clear();
        s->write_buffer.clear();
    }
}

static void real_tm_end() {
    if (tm_nested_call_counter > 1) return;
    CalvinState *s = ensure_state();
    if (s->collecting) {
        s->use_collected = true;
        tm_longjmp_ret = 1;
        siglongjmp(tm_jmpbuf, 1);
        return;
    }
    // Execute phase: validate reads, apply writes.
    for (auto &re : s->read_entries) {
        uint64_t cur = 0;
        memcpy(&cur, re.addr, re.width);
        if (cur != re.captured) {
            s->collecting = true;
            s->use_collected = false;
            s->read_entries.clear();
            s->write_set.clear();
            s->write_buffer.clear();
            siglongjmp(tm_jmpbuf, 1);
        }
    }
    for (auto &w : s->write_set) {
        memcpy(w.addr, &w.value, w.width);
    }
    s->use_collected = false;
}

static void real_tm_abort() {
    CalvinState *s = ensure_state();
    s->collecting = true;
    s->use_collected = false;
    s->read_entries.clear();
    s->write_set.clear();
    s->write_buffer.clear();
}

static uint64_t read_tracked(void *addr, uint8_t width) {
    CalvinState *s = ensure_state();
    if (s->collecting) {
        uint64_t val = 0;
        memcpy(&val, addr, width);
        s->read_entries.push_back({addr, val, width});
        return val;
    }
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
    if (s->collecting) {
        uint64_t prev = 0;
        memcpy(&prev, addr, width);
        s->read_entries.push_back({addr, prev, width});
    }
    s->write_buffer[addr] = {addr, val, width};
    s->write_set.push_back({addr, val, width});
}

static uint8_t  real_tm_read_i1 (int8_t  *a) { LLVM_TM_ADDR_CHECK(a); return (uint8_t) read_tracked((void*)a, 1); }
static uint16_t real_tm_read_i2 (int16_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint16_t)read_tracked((void*)a, 2); }
static uint32_t real_tm_read_i4 (int32_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint32_t)read_tracked((void*)a, 4); }
static uint64_t real_tm_read_i8 (int64_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint64_t)read_tracked((void*)a, 8); }
static float    real_tm_read_f4 (float   *a) { LLVM_TM_ADDR_CHECK(a); float v; uint32_t tmp = (uint32_t)read_tracked((void*)a, 4); memcpy(&v, &tmp, 4); return v; }
static double   real_tm_read_f8 (double  *a) { LLVM_TM_ADDR_CHECK(a); double v; uint64_t tmp = read_tracked((void*)a, 8); memcpy(&v, &tmp, 8); return v; }

static void real_tm_write_i1(int8_t  *a, uint8_t  v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 1); }
static void real_tm_write_i2(int16_t *a, uint16_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 2); }
static void real_tm_write_i4(int32_t *a, uint32_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 4); }
static void real_tm_write_i8(int64_t *a, uint64_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v, 8); }
static void real_tm_write_f4(float   *a, float    v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); uint32_t tmp; memcpy(&tmp, &v, 4); write_tracked((void*)a, tmp, 4); }
static void real_tm_write_f8(double  *a, double   v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); uint64_t tmp; memcpy(&tmp, &v, 8); write_tracked((void*)a, tmp, 8); }

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
static void real_tm_free(void *p) { if (!p) return; }
static void real_tm_init() { stm::tm_region_init(); }
static void real_tm_exit() {}
static void real_tm_init_thread() { tm_hook_init_thread(); ensure_state(); }
static void real_tm_exit_thread() {}
static void *real_tm_get_thread_state() { return nullptr; }

static TMRealHooks g_calvin_hooks = {
    .begin            = real_tm_begin,
    .end              = real_tm_end,
    .malloc           = real_tm_malloc,
    .calloc           = real_tm_calloc,
    .realloc          = real_tm_realloc,
    .free             = real_tm_free,
    .read_i1          = (uint8_t (*)(uint8_t*))real_tm_read_i1,
    .read_i2          = (uint16_t(*)(uint16_t*))real_tm_read_i2,
    .read_i4          = (uint32_t(*)(uint32_t*))real_tm_read_i4,
    .read_i8          = (uint64_t(*)(uint64_t*))real_tm_read_i8,
    .read_f4          = real_tm_read_f4,
    .read_f8          = real_tm_read_f8,
    .read_ptr         = nullptr,
    .write_i1         = (void(*)(uint8_t*,uint8_t))real_tm_write_i1,
    .write_i2         = (void(*)(uint16_t*,uint16_t))real_tm_write_i2,
    .write_i4         = (void(*)(uint32_t*,uint32_t))real_tm_write_i4,
    .write_i8         = (void(*)(uint64_t*,int64_t))real_tm_write_i8,
    .write_f4         = real_tm_write_f4,
    .write_f8         = real_tm_write_f8,
    .write_ptr        = nullptr,
    .get_env          = nullptr,
    .set_jmpbuf       = nullptr,
    .get_thread_state = real_tm_get_thread_state,
};

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
