#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "tm_thread_state.hpp"
#include "tm_hooks.hpp"

// Forward declarations required by tinystm_wbctl.hpp (included via tinystm_globals.hpp)
extern "C" int tm_serialize_unlock_all();

#include "tinystm_globals.hpp"
#include "tm_alloc_overrides.hpp"
#include "tm_backend_macros.hpp"
#include "dudetm/dudetm_base.hpp"
extern const TMRealHooks g_dudetm_hooks;



// Thread-local redo-log batch: accumulates MALLOC/FREE entries
// during a transaction.  Writes come from the TM write-set at
// commit time.
thread_local std::vector<dudetm::DUDERedoEntry> tls_redo_batch;

extern "C" {

extern uint32_t tm_symbol_count;
extern void*    tm_symbol_addresses[];
extern uint64_t tm_symbol_sizes[];

extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
__thread int tm_init_thread_call_count = 0;

thread_local uint64_t tm_begin_count{0};
thread_local uint64_t tm_end_count{0};
thread_local uint64_t tm_tx_count{0};

std::atomic<uint64_t> g_tm_max_read_set{0};
std::atomic<uint64_t> g_tm_max_write_set{0};
thread_local uint64_t g_tm_tx_read_set{0};
thread_local uint64_t g_tm_tx_write_set{0};
static std::atomic<uint64_t> g_tm_begin_count{0};
static std::atomic<uint64_t> g_tm_end_count{0};
static std::atomic<uint64_t> g_tm_tx_count{0};

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
    tinystm::init();
    dudetm::init(tm_symbol_count,
                 tm_symbol_addresses,
                 tm_symbol_sizes);
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed\n");
        abort();
    }
    tm_register_real_hooks(&g_dudetm_hooks);
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit()
#else
void tm_exit()
#endif
{
    dudetm::shutdown();
    tinystm::exit();
    stm::tm_region_destroy();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_init_thread()
#else
void tm_init_thread()
#endif
{
    tm_hook_init_thread();
    tm_init_thread_call_count++;
    tinystm::init_thread();
}

#ifdef LLVM_TM_PLUGIN
static void do_tm_exit_thread()
#else
void tm_exit_thread()
#endif
{
    tm_hook_exit_thread();
}

// ── TMThreadState ──────────────────────────────────────────
static thread_local TMThreadState g_tm_thread_state{0, 0};
static thread_local sigjmp_buf   g_tm_jmpbuf;

static void *real_tm_get_thread_state() {
    return (void*)&g_tm_thread_state;
}





#define TM_BUFFER_SIZE 1024
static __thread uint8_t tm_buffer[TM_BUFFER_SIZE];
TM_DEFINE_PLUGIN_RW(tinystm)

void tm_load_symbols(void *symbol_table, uint32_t symbol_count) { (void)symbol_table; (void)symbol_count; }

void consume_ptr(volatile void *ptr) { (void)ptr; }

thread_local bool g_tm_expli_mode = false;

} // extern "C"

// ═══════════════════════════════════════════════════════════════════
//  Hook implementations (static; registered via tm_register_real_hooks)
// ═══════════════════════════════════════════════════════════════════

static void
push_alloc_entry(void* ptr, size_t size)
{
    dudetm::DUDERedoEntry e;
    e.op_type = dudetm::OP_MALLOC;
    memset(e._pad, 0, sizeof(e._pad));
    e.addr = reinterpret_cast<uint64_t>(ptr);
    e.val.u8 = (uint64_t)size;
    e.type = stm::ValueType::UINT64;
    tls_redo_batch.push_back(e);
}

static void
push_free_entry(void* ptr)
{
    dudetm::DUDERedoEntry e;
    e.op_type = dudetm::OP_FREE;
    memset(e._pad, 0, sizeof(e._pad));
    e.addr = reinterpret_cast<uint64_t>(ptr);
    e.val.u8 = 0;
    e.type = stm::ValueType::UINT64;
    tls_redo_batch.push_back(e);
}

static void real_tm_begin()
{
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
    tm_begin_count++;
    g_in_tx = true;
    tm_clear_spec_allocs();
    tm_clear_deferred_frees();
    tls_redo_batch.clear();  // clear stale entries from a prior abort
    tinystm::begin();
    assert(tm_nested_call_counter >= 0);
}

static void real_tm_end()
{
    g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
    tm_end_count++;
    g_in_tx = false;
    auto *tx = tinystm::current_tx_wbctl;

    // Snapshot write-set BEFORE commit — commit() calls tx->reset() which clears it.
    std::vector<std::pair<void *, tinystm::WriteLogEntry_wbctl>> local_writes;
    if (tx) {
        uint64_t rs = tx->read_set.size();
        uint64_t ws = tx->write_set.size();
        if (rs > g_tm_max_read_set.load()) g_tm_max_read_set.store(rs);
        if (ws > g_tm_max_write_set.load()) g_tm_max_write_set.store(ws);
        local_writes.reserve(ws);
        for (auto &entry : tx->write_set)
            local_writes.emplace_back(entry.first, entry.second);
    }

    tinystm::commit();  // on abort, longjmps past here — write_set now cleared

    // ── Build committed batch: COMMIT_BEGIN + writes + alloc/free ──
    uint64_t seq = dudetm::g_ctrl->global_commit_seq.fetch_add(
        1, std::memory_order_relaxed) + 1;

    std::vector<dudetm::DUDERedoEntry> batch;
    batch.reserve(1 + local_writes.size() + tls_redo_batch.size());

    dudetm::DUDERedoEntry marker;
    marker.op_type = dudetm::OP_COMMIT_BEGIN;
    memset(marker._pad, 0, sizeof(marker._pad));
    marker.addr = seq;
    marker.val.u8 = 0;
    marker.type = stm::ValueType::UINT64;
    batch.push_back(marker);

    // Write-set entries (from snapshot before commit cleared them)
    for (auto &entry : local_writes) {
        dudetm::DUDERedoEntry re;
        re.op_type = dudetm::OP_WRITE;
        memset(re._pad, 0, sizeof(re._pad));
        re.addr = reinterpret_cast<uint64_t>(entry.first);
        re.val  = entry.second.new_val;
        re.type = entry.second.type;
        batch.push_back(re);
    }

    // Alloc/free entries from the thread-local batch
    batch.insert(batch.end(), tls_redo_batch.begin(), tls_redo_batch.end());
    tls_redo_batch.clear();

    dudetm::publish_batch(batch.data(), batch.size());

    tm_flush_spec_allocs();
    tm_flush_deferred_frees();
    assert(tm_nested_call_counter >= 0);
    tm_tx_count++;
}

TM_DEFINE_READ_WRITE_HOOKS(tinystm)

static void* real_tm_malloc(size_t size)
{
    void* p = stm::tm_region_malloc(size);
    tm_track_alloc_result(p, size);
    if (g_in_tx) push_alloc_entry(p, size);
    return p;
}

static void* real_tm_calloc(size_t nmemb, size_t size)
{
    void* p = stm::tm_region_malloc(nmemb * size);
    memset(p, 0, nmemb * size);
    tm_track_alloc_result(p, nmemb * size);
    if (g_in_tx) push_alloc_entry(p, nmemb * size);
    return p;
}

static void* real_tm_realloc(void* ptr, size_t size)
{
    void* p = stm::tm_region_malloc(size);
    if (ptr) {
        memcpy(p, ptr, size);
        if (g_in_tx) push_free_entry(ptr);
        stm::tm_region_free(ptr);
    }
    tm_track_alloc_result(p, size);
    if (g_in_tx) push_alloc_entry(p, size);
    return p;
}

static void real_tm_free(void* ptr)
{
    if (!ptr || !stm::isTMAddress(ptr)) return;
    TM_EVENT(FREE, ptr, 0);
    if (g_in_tx) {
        tinystm::tm_write_i1(reinterpret_cast<uint8_t*>(ptr), 0);
        tm_free_append_deferred(ptr);
        push_free_entry(ptr);
    } else {
        stm::tm_region_free(ptr);
    }
}

static void *real_tm_get_env() { return (void*)&g_tm_jmpbuf; }

static void real_tm_set_jmpbuf(void *buf) { tinystm::jmpbuf = (sigjmp_buf *)buf; }

// ═══════════════════════════════════════════════════════════════════
//  Hook registration table
// ═══════════════════════════════════════════════════════════════════

TM_REAL_HOOKS_TABLE_EXT(dudetm)
