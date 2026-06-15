#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include "tm_alloc_overrides.hpp"
#include "tm_thread_state.hpp"
thread_local bool g_in_tx = false;
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "rel_ptr.hpp"
#include "tm_hooks.hpp"

extern const TMRealHooks g_psgl_hooks;

static std::mutex global_tx_lock;
static std::atomic<bool> initialized{false};
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_tx_count{0};

extern "C" {
extern __thread int32_t    tm_nested_call_counter;
extern __thread int32_t    tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}
static thread_local TMThreadState g_tm_persist_state{0, 0};
static std::recursive_mutex g_serialize_mutex;

static constexpr size_t PERSIST_HEAP_SIZE = 64UL * 1024 * 1024; // 64 MB

static const char* PERSIST_FILE = "benchmark_results/tm_persist.bin";

static uint8_t* g_mmap_base = nullptr;
static size_t g_mmap_size = 0;
static size_t heap_start_off_global = 0;  // offset of allocator heap in mmap

struct SymbolRange {
    uintptr_t addr_start;
    uintptr_t addr_end;
    size_t file_off;
};
static SymbolRange* g_sym_ranges = nullptr;
static uint32_t g_sym_count = 0;

static size_t addr_to_file_off(uintptr_t addr) {
    if (g_sym_count == 0) return (size_t)-1;
    int lo = 0, hi = (int)g_sym_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr < g_sym_ranges[mid].addr_start)
            hi = mid - 1;
        else if (addr >= g_sym_ranges[mid].addr_end)
            lo = mid + 1;
        else
            return g_sym_ranges[mid].file_off + (addr - g_sym_ranges[mid].addr_start);
    }
    return (size_t)-1;
}

static void persist_write(size_t file_off, const void* val, size_t sz) {
    if (g_mmap_base && file_off != (size_t)-1)
        memcpy(g_mmap_base + file_off, val, sz);
}

// ── Real hook implementations ─────────────────────────────────

static void real_tm_begin() {
    g_in_tx = true;
    g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
    global_tx_lock.lock();
}

static void real_tm_end() {
    g_in_tx = false;
    g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
    g_tm_tx_count.fetch_add(1, std::memory_order_relaxed);
    global_tx_lock.unlock();
}

static uint8_t real_tm_read_i1(uint8_t* addr) { return *addr; }
static uint16_t real_tm_read_i2(uint16_t* addr) { return *addr; }
static uint32_t real_tm_read_i4(uint32_t* addr) { return *addr; }
static uint64_t real_tm_read_i8(uint64_t* addr) { return *addr; }
static float real_tm_read_f4(float* addr) { return *addr; }
static double real_tm_read_f8(double* addr) { return *addr; }
static void* real_tm_read_ptr(void** addr) { return (void*)*addr; }

static void real_tm_write_i1(uint8_t* addr, uint8_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 1);
}

static void real_tm_write_i2(uint16_t* addr, uint16_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 2);
}

static void real_tm_write_i4(uint32_t* addr, uint32_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 4);
}

static void real_tm_write_i8(uint64_t* addr, int64_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 8);
}

static void real_tm_write_f4(float* addr, float val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 4);
}

static void real_tm_write_f8(double* addr, double val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 8);
}

static void real_tm_write_ptr(void** addr, void* val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, sizeof(void*));
}

// TM allocator stubs (redirect to system allocator)
// ── Persistent allocator ────────────────────────────────────
// Bump-allocates from the persistent mmap region so allocations
// survive process restarts.  tm_free is a no-op (bump allocator).

static void* real_tm_malloc(size_t size) {
    if (!g_mmap_base || size == 0) return malloc(size);
    if (g_in_tx) {
        // Bump-allocate from the persistent heap
        uint64_t old_bump = __atomic_fetch_add(
            reinterpret_cast<uint64_t*>(g_mmap_base + 16), size, __ATOMIC_RELAXED);
        if (old_bump + size > PERSIST_HEAP_SIZE) {
            fprintf(stderr, "PersistentSGL: OOM (%zu > %zu)\n",
                    old_bump + size, PERSIST_HEAP_SIZE);
            return malloc(size);
        }
        return g_mmap_base + heap_start_off_global + old_bump;
    }
    return malloc(size);
}

static void* real_tm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* p = real_tm_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void* real_tm_realloc(void* ptr, size_t size) {
    if (!ptr) return real_tm_malloc(size);
    if (!g_mmap_base) return realloc(ptr, size);
    // Check if ptr is in the persistent heap
    uint8_t* heap_start = g_mmap_base + heap_start_off_global;
    if (ptr >= (void*)heap_start && ptr < (void*)(heap_start + PERSIST_HEAP_SIZE)) {
        // In persistent heap: allocate new, copy (bump allocator can't shrink/grow in place)
        void* newp = real_tm_malloc(size);
        if (newp && size > 0) {
            // Copy old data (up to min(old_size, new_size))
            // We don't know old_size, copy up to new_size
            memcpy(newp, ptr, size);
        }
        return newp;
    }
    return realloc(ptr, size);
}

static void real_tm_free(void* ptr) {
    if (!g_mmap_base || !ptr) return;
    uint8_t* heap_start = g_mmap_base + heap_start_off_global;
    if (ptr >= (void*)heap_start && ptr < (void*)(heap_start + PERSIST_HEAP_SIZE)) {
        return; // persistent heap: no-op (bump allocator)
    }
    free(ptr); // system heap
}

const TMRealHooks g_psgl_hooks = {
    real_tm_begin,
    real_tm_end,
    real_tm_malloc,
    real_tm_calloc,
    real_tm_realloc,
    real_tm_free,
    real_tm_read_i1,
    real_tm_read_i2,
    real_tm_read_i4,
    real_tm_read_i8,
    real_tm_read_f4,
    real_tm_read_f8,
    real_tm_read_ptr,
    real_tm_write_i1,
    real_tm_write_i2,
    real_tm_write_i4,
    real_tm_write_i8,
    real_tm_write_f4,
    real_tm_write_f8,
    real_tm_write_ptr,
};

// ── Extern "C" API ────────────────────────────────────────────

extern "C" {

TMThreadState *tm_get_thread_state() {
    return &g_tm_persist_state;
}

extern uint32_t tm_symbol_count;
extern const char* tm_symbol_names[];
extern void* tm_symbol_addresses[];
extern uint64_t tm_symbol_sizes[];

void tm_init() {
    if (stm::tm_region_init() != 0) {
        fprintf(stderr, "FATAL: tm_region_init() failed\n");
        abort();
    }
    tm_register_real_hooks(&g_psgl_hooks);

    uint32_t n = tm_symbol_count;
    if (n == 0) {
        initialized.store(true, std::memory_order_seq_cst);
        return;
    }

    uint64_t data_size = 0;
    for (uint32_t i = 0; i < n; i++)
        data_size += tm_symbol_sizes[i];

    size_t hdr_size = 24; // magic(8) + version(4) + count(4) + heap_bump(8)
    g_mmap_size = hdr_size + data_size + PERSIST_HEAP_SIZE;

    // Offset within the mmap where the allocator heap starts
    heap_start_off_global = hdr_size + data_size;

    int fd = open(PERSIST_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("PersistentSGL: open");
        exit(1);
    }

    struct stat st;
    bool file_has_data = (fstat(fd, &st) == 0 && (size_t)st.st_size >= g_mmap_size);

// Fixed mmap address for deterministic VA across restarts.
// 0x600000000000 is in the mmap range on 64-bit systems and unlikely
// to conflict with other mappings.  This makes std::map's internal
// node pointers valid across restarts.
static constexpr uintptr_t PERSIST_MMAP_FIXED_ADDR = 0x600000000000ULL;

    if (ftruncate(fd, g_mmap_size) < 0) {
        perror("PersistentSGL: ftruncate");
        close(fd);
        exit(1);
    }

    // Try fixed-address mmap first, fall back to any address
    g_mmap_base = (uint8_t*)mmap(
        (void*)PERSIST_MMAP_FIXED_ADDR, g_mmap_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_FIXED, fd, 0);
    if (g_mmap_base == MAP_FAILED) {
        // Fall back to any address (ASLR will change it, breaking
        // pointer-based data structures like std::map across restarts).
        g_mmap_base = (uint8_t*)mmap(
            nullptr, g_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (g_mmap_base == MAP_FAILED) {
            perror("PersistentSGL: mmap");
            close(fd);
            exit(1);
        }
        fprintf(stderr, "PersistentSGL: WARNING: mmap at 0x%llx failed, using ASLR address %p\n"
                "  std::map and similar pointer-based containers will NOT persist correctly.\n",
                (unsigned long long)PERSIST_MMAP_FIXED_ADDR, (void*)g_mmap_base);
    } else {
        fprintf(stderr, "PersistentSGL: mmap at fixed address %p\n", (void*)g_mmap_base);
    }
    close(fd);

    // Set RelPtr base so offset_ptr works in persisted data
    RelPtr<void>::set_base(g_mmap_base);

    uint64_t magic = 0x53524550524D54ULL;
    uint32_t version = 2;  // version 2: added allocator heap

    if (file_has_data) {
        uint64_t fmagic;
        memcpy(&fmagic, g_mmap_base, 8);
        uint32_t fver, fcount;
        memcpy(&fver, g_mmap_base + 8, 4);
        memcpy(&fcount, g_mmap_base + 12, 4);
        if (fmagic == magic && fver == version && fcount == n) {
            uint64_t off = hdr_size;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t sz = tm_symbol_sizes[i];
                void* addr = tm_symbol_addresses[i];
                memcpy(addr, g_mmap_base + off, sz);
                off += sz;
            }
            fprintf(stderr, "PersistentSGL: restored %u symbols from %s\n", n, PERSIST_FILE);
        } else {
            fprintf(stderr, "PersistentSGL: file format mismatch, reinitializing\n");
            memcpy(g_mmap_base, &magic, 8);
            memcpy(g_mmap_base + 8, &version, 4);
            memcpy(g_mmap_base + 12, &n, 4);
            // Reset allocator heap bump offset to 0
            uint64_t bump_zero = 0;
            memcpy(g_mmap_base + 16, &bump_zero, 8);
            uint64_t off = hdr_size;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t sz = tm_symbol_sizes[i];
                void* addr = tm_symbol_addresses[i];
                memcpy(g_mmap_base + off, addr, sz);
                off += sz;
            }
            msync(g_mmap_base, g_mmap_size, MS_SYNC);
        }
    } else {
        memcpy(g_mmap_base, &magic, 8);
        memcpy(g_mmap_base + 8, &version, 4);
        memcpy(g_mmap_base + 12, &n, 4);
        uint64_t bump_zero = 0;
        memcpy(g_mmap_base + 16, &bump_zero, 8);
        uint64_t off = hdr_size;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t sz = tm_symbol_sizes[i];
            void* addr = tm_symbol_addresses[i];
            memcpy(g_mmap_base + off, addr, sz);
            off += sz;
        }
        msync(g_mmap_base, g_mmap_size, MS_SYNC);
        fprintf(stderr, "PersistentSGL: initialized new %s\n", PERSIST_FILE);
    }

    g_sym_ranges = new SymbolRange[n];
    g_sym_count = n;
    uint64_t off = hdr_size;
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t start = (uintptr_t)tm_symbol_addresses[i];
        uint64_t sz = tm_symbol_sizes[i];
        g_sym_ranges[i].addr_start = start;
        g_sym_ranges[i].addr_end = start + sz;
        g_sym_ranges[i].file_off = off;
        off += sz;
    }

    initialized.store(true, std::memory_order_seq_cst);
}

void tm_init_thread() {
    tm_hook_init_thread();
}

void tm_exit() {
    if (g_mmap_base) {
        // Save final TM symbol state to the mmap file before unmapping.
        // This captures changes made both inside TX functions (via tm_write_*)
        // and outside them (direct memory writes to TM globals).
        for (uint32_t i = 0; i < g_sym_count; i++) {
            memcpy(g_mmap_base + g_sym_ranges[i].file_off,
                   (void*)g_sym_ranges[i].addr_start,
                   g_sym_ranges[i].addr_end - g_sym_ranges[i].addr_start);
        }
        msync(g_mmap_base, g_mmap_size, MS_SYNC);
        munmap(g_mmap_base, g_mmap_size);
        g_mmap_base = nullptr;
    }
    delete[] g_sym_ranges;
    g_sym_ranges = nullptr;
    g_sym_count = 0;
    initialized.store(false, std::memory_order_seq_cst);
}

void tm_exit_thread() {
    tm_hook_exit_thread();
}

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() { return 0; }



void tm_set_env(sigjmp_buf* env) {
    if (env)
        memcpy(&tm_jmpbuf, env, sizeof(sigjmp_buf));
}

void tm_load_symbols(void* symbol_table, uint32_t symbol_count) {}

void tm_read_i16(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    out_words[0] = vaddr[0];
    out_words[1] = vaddr[1];
}
void tm_read_i32(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 4; i++) out_words[i] = vaddr[i];
}
void tm_read_i64(void *addr, void *out) {
    auto *out_words = static_cast<uint64_t *>(out);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 8; i++) out_words[i] = vaddr[i];
}

void* tm_read_z(volatile uint8_t* src, uint64_t len) {
    void* buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

void tm_write_i16(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 2; i++) {
        size_t off = addr_to_file_off((uintptr_t)(vaddr + i));
        vaddr[i] = val_words[i];
        if (off != (size_t)-1) persist_write(off, &val_words[i], 8);
    }
}
void tm_write_i32(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 4; i++) {
        size_t off = addr_to_file_off((uintptr_t)(vaddr + i));
        vaddr[i] = val_words[i];
        if (off != (size_t)-1) persist_write(off, &val_words[i], 8);
    }
}
void tm_write_i64(void *addr, void *val) {
    auto *val_words = static_cast<const uint64_t *>(val);
    auto *vaddr = static_cast<volatile uint64_t *>(addr);
    for (int i = 0; i < 8; i++) {
        size_t off = addr_to_file_off((uintptr_t)(vaddr + i));
        vaddr[i] = val_words[i];
        if (off != (size_t)-1) persist_write(off, &val_words[i], 8);
    }
}

void tm_write_z(volatile uint8_t* dst, volatile uint8_t* src, uint64_t len) {
    size_t off = addr_to_file_off((uintptr_t)dst);
    memcpy((void*)dst, (const void*)src, len);
    if (off != (size_t)-1) persist_write(off, (const void*)src, len);
}

void tm_memset(volatile uint8_t* addr, uint8_t val, uint64_t len) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    memset((void*)addr, val, len);
    if (off != (size_t)-1) {
        uint8_t* buf = (uint8_t*)malloc(len);
        memset(buf, val, len);
        persist_write(off, buf, len);
        free(buf);
    }
}

void consume_ptr(volatile void* ptr) { (void)ptr; }

} // extern "C"
