#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../rel_ptr.hpp"

static std::mutex global_tx_lock;
static std::atomic<bool> initialized{false};
static std::atomic<int64_t> g_tm_begin_count{0};
static std::atomic<int64_t> g_tm_end_count{0};
static std::atomic<int64_t> g_tm_tx_count{0};

__thread std::jmp_buf tm_jmpbuf;
__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;

static const char* PERSIST_FILE = "benchmark_results/tm_persist.bin";

static uint8_t* g_mmap_base = nullptr;
static size_t g_mmap_size = 0;

struct SymbolRange {
    uintptr_t addr_start;
    uintptr_t addr_end;
    size_t file_off;
};
static SymbolRange* g_sym_ranges = nullptr;
static uint32_t g_sym_count = 0;

extern "C" {
extern uint32_t tm_symbol_count;
extern const char* tm_symbol_names[];
extern void* tm_symbol_addresses[];
extern uint64_t tm_symbol_sizes[];

void tm_init() {
    fprintf(stderr, "PersistentSGL: tm_init called\n");
    fflush(stderr);

    uint32_t n = tm_symbol_count;
    if (n == 0) {
        initialized.store(true, std::memory_order_seq_cst);
        return;
    }

    uint64_t data_size = 0;
    for (uint32_t i = 0; i < n; i++)
        data_size += tm_symbol_sizes[i];

    size_t hdr_size = 8 + 4 + 4;
    g_mmap_size = hdr_size + data_size;

    int fd = open(PERSIST_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("PersistentSGL: open");
        exit(1);
    }

    struct stat st;
    bool file_has_data = (fstat(fd, &st) == 0 && (size_t)st.st_size >= g_mmap_size);

    if (ftruncate(fd, g_mmap_size) < 0) {
        perror("PersistentSGL: ftruncate");
        close(fd);
        exit(1);
    }

    g_mmap_base = (uint8_t*)mmap(nullptr, g_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_mmap_base == MAP_FAILED) {
        perror("PersistentSGL: mmap");
        close(fd);
        exit(1);
    }
    close(fd);

    // Set RelPtr base so offset_ptr works in persisted data
    RelPtr<void>::set_base(g_mmap_base);

    uint64_t magic = 0x53524550524D54ULL;
    uint32_t version = 1;

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
    fprintf(stderr, "PersistentSGL: tm_init done\n");
    fflush(stderr);
}

void tm_init_thread() {
    fprintf(stderr, "PersistentSGL: tm_init_thread called\n");
    fflush(stderr);
}

void tm_exit() {
    if (g_mmap_base) {
        msync(g_mmap_base, g_mmap_size, MS_SYNC);
        munmap(g_mmap_base, g_mmap_size);
        g_mmap_base = nullptr;
    }
    delete[] g_sym_ranges;
    g_sym_ranges = nullptr;
    g_sym_count = 0;
    initialized.store(false, std::memory_order_seq_cst);
    fprintf(stderr, "PersistentSGL: tm_exit done\n");
}

void tm_exit_thread() {}

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock() { g_serialize_mutex.lock(); }

void tm_serialize_unlock() { g_serialize_mutex.unlock(); }

int tm_setjmp() { return 0; }

void tm_set_jmpbuf(void *buf) { }

sigjmp_buf* tm_get_env() {
    return (sigjmp_buf*)&tm_jmpbuf;
}

void tm_set_env(sigjmp_buf* env) {
    if (env)
        memcpy(&tm_jmpbuf, env, sizeof(tm_jmpbuf));
}

void tm_load_symbols(void* symbol_table, uint32_t symbol_count) {}

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

void tm_begin() {
    if (tm_nested_call_counter == 1) {
        g_tm_begin_count.fetch_add(1, std::memory_order_relaxed);
        global_tx_lock.lock();
    }
}

void tm_end() {
    if (tm_nested_call_counter == 1) {
        g_tm_end_count.fetch_add(1, std::memory_order_relaxed);
        g_tm_tx_count.fetch_add(1, std::memory_order_relaxed);
        global_tx_lock.unlock();
    }
}

uint8_t tm_read_i1(volatile uint8_t* addr) { return *addr; }
uint16_t tm_read_i2(volatile uint16_t* addr) { return *addr; }
uint32_t tm_read_i4(volatile uint32_t* addr) { return *addr; }
uint64_t tm_read_i8(volatile uint64_t* addr) { return *addr; }
float tm_read_f4(volatile float* addr) { return *addr; }
double tm_read_f8(volatile double* addr) { return *addr; }
void* tm_read_ptr(volatile void** addr) { return (void*)*addr; }

void* tm_read_z(volatile uint8_t* src, uint64_t len) {
    void* buf = malloc(len);
    memcpy(buf, (const void*)src, len);
    return buf;
}

void tm_write_i1(volatile uint8_t* addr, uint8_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 1);
}

void tm_write_i2(volatile uint16_t* addr, uint16_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 2);
}

void tm_write_i4(volatile uint32_t* addr, uint32_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 4);
}

void tm_write_i8(volatile uint64_t* addr, uint64_t val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 8);
}

void tm_write_f4(volatile float* addr, float val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 4);
}

void tm_write_f8(volatile double* addr, double val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, 8);
}

void tm_write_ptr(volatile void** addr, void* val) {
    size_t off = addr_to_file_off((uintptr_t)addr);
    *addr = val;
    if (off != (size_t)-1) persist_write(off, &val, sizeof(void*));
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

static void print_stats() {
    fprintf(stderr, "=== PersistentSGL Runtime Stats ===\n");
    fprintf(stderr,
            "tm_begin: %lld, tm_end: %lld, #TXs: %lld\n",
            (long long)g_tm_begin_count.load(std::memory_order_relaxed),
            (long long)g_tm_end_count.load(std::memory_order_relaxed),
            (long long)g_tm_tx_count.load(std::memory_order_relaxed));
}

static int init = (std::atexit(print_stats), 0);

} // extern "C"
