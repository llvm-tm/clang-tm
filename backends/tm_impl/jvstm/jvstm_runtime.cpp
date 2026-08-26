#include <atomic>
#include <cstring>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

// ── VBox: Versioned Box ────────────────────────────────────
// Each body is an immutable node in a singly-linked list,
// ordered newest-first (prepend on commit).
struct VBoxBody {
    uint64_t  version;
    uint64_t  value;        // stored as uint64_t (covers all data types)
    VBoxBody *next;         // immutable link to older body
};

struct VBox {
    std::atomic<VBoxBody*> head;
};

// ── Global state ───────────────────────────────────────────
static std::mutex g_box_mutex;
static std::unordered_map<void*, VBox*> g_boxes;
static std::mutex g_commit_lock;
static std::atomic<uint64_t> g_clock{0};

// ── Per-thread state ───────────────────────────────────────
struct JvstmReadEntry {
    void    *addr;
    uint64_t captured_version;
};

struct JvstmWriteEntry {
    void    *addr;
    uint64_t value;
};

static thread_local uint64_t t_rv = 0;
static thread_local std::vector<JvstmReadEntry> t_read_set;
static thread_local std::vector<JvstmWriteEntry> t_write_set;
static thread_local bool t_has_write = false;

// ── VBox helpers ───────────────────────────────────────────
static VBox *get_or_create_vbox(void *addr) {
    std::lock_guard<std::mutex> lock(g_box_mutex);
    auto it = g_boxes.find(addr);
    if (it != g_boxes.end()) return it->second;
    // Create VBox with initial body (version=0, value=current memory)
    uint64_t initial_val = 0;
    memcpy(&initial_val, addr, sizeof(uint64_t));
    VBoxBody *body = new VBoxBody{0, initial_val, nullptr};
    VBox *box = new VBox{body};
    g_boxes[addr] = box;
    return box;
}

static VBoxBody *find_body(VBox *box, uint64_t rv) {
    // Walk head → newer bodies; the first with version ≤ rv is correct
    // (newest-first ordering).
    VBoxBody *cur = box->head.load(std::memory_order_acquire);
    while (cur) {
        if (cur->version <= rv) return cur;
        cur = cur->next;
    }
    // Should never happen: the initial body has version 0 ≤ rv
    return nullptr;
}

// ── TM operations ──────────────────────────────────────────
static void real_tm_begin() {
    if (tm_nested_call_counter > 1) return;
    t_rv = g_clock.load(std::memory_order_acquire);
    t_read_set.clear();
    t_write_set.clear();
    t_has_write = false;
}

static void real_tm_end() {
    if (tm_nested_call_counter > 1) return;
    if (!t_has_write) {
        // Read-only: no lock, no validation
        return;
    }
    // Write commit: acquire lock, validate, prepend, unlock
    g_commit_lock.lock();
    uint64_t ct = g_clock.fetch_add(1, std::memory_order_acq_rel) + 1;

    // Validate read-set: each VBox head version must match captured
    bool valid = true;
    for (auto &re : t_read_set) {
        VBox *box = get_or_create_vbox(re.addr);
        VBoxBody *head = box->head.load(std::memory_order_acquire);
        if (head->version != re.captured_version) {
            valid = false;
            break;
        }
    }
    // Validate write-set: each VBox head version must be ≤ rv
    if (valid) {
        for (auto &we : t_write_set) {
            VBox *box = get_or_create_vbox(we.addr);
            VBoxBody *head = box->head.load(std::memory_order_acquire);
            if (head->version > t_rv) {
                valid = false;
                break;
            }
        }
    }
    if (valid) {
        // Prepend new bodies to each written VBox AND write-through to memory
        // so that .peek() (direct memory read) sees latest committed value
        for (auto &we : t_write_set) {
            VBox *box = get_or_create_vbox(we.addr);
            VBoxBody *new_body = new VBoxBody{ct, we.value, box->head.load(std::memory_order_relaxed)};
            box->head.store(new_body, std::memory_order_release);
            // Write-through: update the original memory location
            memcpy(we.addr, &we.value, sizeof(uint64_t));
        }
        g_commit_lock.unlock();
    } else {
        g_commit_lock.unlock();
        // abort: retry from begin
        t_read_set.clear();
        t_write_set.clear();
        t_has_write = false;
        tm_longjmp_ret = 1;
        siglongjmp(tm_jmpbuf, 1);
    }
}

static void real_tm_abort() {
    t_read_set.clear();
    t_write_set.clear();
    t_has_write = false;
}

// ── Tracked read/write ─────────────────────────────────────
static uint64_t read_tracked(void *addr) {
    // Check write-set first (read-own-writes)
    for (auto &we : t_write_set) {
        if (we.addr == addr) return we.value;
    }
    VBox *box = get_or_create_vbox(addr);
    VBoxBody *body = find_body(box, t_rv);
    t_read_set.push_back({addr, body->version});
    return body->value;
}

static void write_tracked(void *addr, uint64_t val) {
    for (auto &we : t_write_set) {
        if (we.addr == addr) {
            we.value = val;
            return;
        }
    }
    t_write_set.push_back({addr, val});
    t_has_write = true;
}

static uint8_t  real_tm_read_i1 (int8_t  *a) { LLVM_TM_ADDR_CHECK(a); return (uint8_t) read_tracked((void*)a); }
static uint16_t real_tm_read_i2 (int16_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint16_t)read_tracked((void*)a); }
static uint32_t real_tm_read_i4 (int32_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint32_t)read_tracked((void*)a); }
static uint64_t real_tm_read_i8 (int64_t *a) { LLVM_TM_ADDR_CHECK(a); return (uint64_t)read_tracked((void*)a); }
static float    real_tm_read_f4 (float   *a) { LLVM_TM_ADDR_CHECK(a); float v; uint32_t tmp = (uint32_t)read_tracked((void*)a); memcpy(&v, &tmp, 4); return v; }
static double   real_tm_read_f8 (double  *a) { LLVM_TM_ADDR_CHECK(a); double v; uint64_t tmp = read_tracked((void*)a); memcpy(&v, &tmp, 8); return v; }

static void real_tm_write_i1(int8_t  *a, uint8_t  v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v); }
static void real_tm_write_i2(int16_t *a, uint16_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v); }
static void real_tm_write_i4(int32_t *a, uint32_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v); }
static void real_tm_write_i8(int64_t *a, uint64_t v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); write_tracked((void*)a, v); }
static void real_tm_write_f4(float   *a, float    v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); uint32_t tmp; memcpy(&tmp, &v, 4); write_tracked((void*)a, tmp); }
static void real_tm_write_f8(double  *a, double   v) { LLVM_TM_ADDR_CHECK_WRITE(a, v); uint64_t tmp; memcpy(&tmp, &v, 8); write_tracked((void*)a, tmp); }

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
static void real_tm_init_thread() { tm_hook_init_thread(); }
static void real_tm_exit_thread() {}
static void *real_tm_get_thread_state() { return nullptr; }

static TMRealHooks g_jvstm_hooks = {
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
void tm_init()        { real_tm_init();        tm_register_real_hooks(&g_jvstm_hooks); }
void tm_exit()        { real_tm_exit(); }
void tm_init_thread() { real_tm_init_thread(); }
void tm_exit_thread() { real_tm_exit_thread(); }
}
#endif
