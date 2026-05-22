#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../tm_common.hpp"

namespace dudetm
{

// ── Entry op-types ─────────────────────────────────────────────
enum EntryOpType : uint8_t {
    OP_WRITE        = 0,
    OP_COMMIT_BEGIN = 1,
    OP_MALLOC       = 2,
    OP_FREE         = 3,
};

// ── Redo Log Entry ─────────────────────────────────────────────
// WRITE:        op_type=OP_WRITE, addr=VA, val=value, type=ValueType
// COMMIT_BEGIN: op_type=OP_COMMIT_BEGIN, addr=seq_no
// MALLOC:       op_type=OP_MALLOC, addr=allocated VA, val.u8=size
// FREE:         op_type=OP_FREE, addr=freed VA

struct DUDERedoEntry {
    uint8_t          op_type;
    uint8_t          _pad[7];
    uint64_t         addr;
    stm::any_type_t  val;
    stm::ValueType   type;
};

// sizeof(DUDERedoEntry) = 32 (1 + 7 pad + 8 + 8 + 1 + 7 trailing pad)

// ── Circular buffer: power of 2 ────────────────────────────────
constexpr size_t DUDEREDOLOG_SIZE  = 1UL << 20;   // 1,048,576 entries
constexpr size_t DUDEREDOLOG_MASK  = DUDEREDOLOG_SIZE - 1;
constexpr uint64_t DUDE_MAX_PENDING = DUDEREDOLOG_SIZE / 2;

// ── Persistent storage ─────────────────────────────────────────
constexpr size_t DUDE_PERSIST_HEAP_SIZE = 64UL * 1024 * 1024;
static constexpr uintptr_t DUDE_PERSIST_MMAP_ADDR = 0x600000000000ULL;

static constexpr const char* DUDE_PERSIST_FILE = "benchmark_results/dudetm_persist.bin";
static constexpr const char* DUDE_REDOLOG_FILE = "benchmark_results/dudetm_redolog.bin";

// ── Shared state (in redolog mmap) ─────────────────────────────
struct DUDEShared {
    std::atomic<uint64_t> head {0};
    std::atomic<uint64_t> tail {0};
    std::atomic<uint64_t> global_commit_seq {0};
    uint8_t               _pad[40];
    DUDERedoEntry         entries[DUDEREDOLOG_SIZE];
};

// ── Global pointers (set by tm_init) ───────────────────────────
static DUDEShared*  g_shared        = nullptr;
static uint8_t*     g_persist_base  = nullptr;
static size_t       g_persist_size  = 0;
static size_t       g_heap_start_off = 0;
static pid_t        g_replayer_pid  = 0;
static volatile bool g_shutdown     = false;

// ── Symbol tracking ────────────────────────────────────────────
struct DUDESymbolRange {
    uintptr_t addr_start;
    uintptr_t addr_end;
    size_t    file_off;
};
static DUDESymbolRange* g_sym_ranges = nullptr;
static uint32_t         g_sym_count  = 0;

// ── Signal handler ─────────────────────────────────────────────
static void dudetm_signal_handler(int) { g_shutdown = true; }

// ── Batch publish (atomically reserve N slots, fill, commit) ───
inline void
publish_batch(const DUDERedoEntry* entries, size_t count)
{
    if (count == 0) return;

    uint64_t h;
    uint64_t t;
    do {
        h = g_shared->head.load(std::memory_order_relaxed);
        t = g_shared->tail.load(std::memory_order_acquire);
    } while (h - t + count > DUDE_MAX_PENDING);

    for (size_t i = 0; i < count; i++)
        g_shared->entries[(h + i) & DUDEREDOLOG_MASK] = entries[i];

    std::atomic_thread_fence(std::memory_order_release);
    g_shared->head.store(h + count, std::memory_order_release);
}

// ── Address → file offset ─────────────────────────────────────
static inline size_t
addr_to_file_off(uintptr_t addr)
{
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

// ── Replayer loop (runs in forked child) ───────────────────────
inline void
replayer_loop()
{
    int fd = open(DUDE_PERSIST_FILE, O_RDWR, 0644);
    if (fd < 0) { perror("dudetm replayer: open persist"); _exit(1); }

    uint8_t* local_persist = (uint8_t*)mmap(
        (void*)DUDE_PERSIST_MMAP_ADDR, g_persist_size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (local_persist == MAP_FAILED) {
        perror("dudetm replayer: mmap persist");
        close(fd); _exit(1);
    }
    close(fd);

    // Track which addresses have been allocated within the
    // persistent region (populated by replaying MALLOC/FREE).
    // On crash recovery the persistent file itself holds the
    // authoritative state, so this is a best-effort tracking set.
    static constexpr size_t BITMAP_SIZE = 65536;
    static uint64_t alloc_bitmap[BITMAP_SIZE / 64] = {0};

    fprintf(stderr, "[DUDETM] replayer started, pid=%d\n", getpid());
    fflush(stderr);

    uint64_t next_expected_seq = 1;
    uint64_t last_sync = 0;

    while (!g_shutdown) {
        uint64_t h = g_shared->head.load(std::memory_order_acquire);
        uint64_t t = g_shared->tail.load(std::memory_order_acquire);

        if (t == h) {
            usleep(100);
            continue;
        }

        while (t < h && !g_shutdown) {
            DUDERedoEntry e = g_shared->entries[t & DUDEREDOLOG_MASK];

            switch (e.op_type) {

            case OP_COMMIT_BEGIN: {
                uint64_t seq = e.addr;
                if (seq > next_expected_seq) {
                    // Out-of-order commit — shouldn't happen with a single
                    // shared circular buffer, but guard anyway.
                    usleep(10);
                    continue;
                }
                next_expected_seq = seq + 1;
                break;
            }

            case OP_WRITE: {
                stm::write_value_to_addr(
                    reinterpret_cast<void*>(e.addr), e.val, e.type);
                // Also persist to file-backed region
                size_t off = addr_to_file_off(e.addr);
                if (off != (size_t)-1) {
                    size_t sz = 8;
                    switch (e.type) {
                    case stm::ValueType::UINT8:  sz = 1; break;
                    case stm::ValueType::UINT16: sz = 2; break;
                    case stm::ValueType::UINT32:
                    case stm::ValueType::FLOAT:  sz = 4; break;
                    default: break;
                    }
                    memcpy(local_persist + off,
                           reinterpret_cast<void*>(e.addr), sz);
                }
                break;
            }

            case OP_MALLOC: {
                // Track the allocated address in the bitmap
                uint64_t idx = (e.addr - DUDE_PERSIST_MMAP_ADDR) / 64;
                if (idx < BITMAP_SIZE / 64)
                    __sync_fetch_and_or(&alloc_bitmap[idx / 64],
                                        1ULL << (idx % 64));
                break;
            }

            case OP_FREE: {
                uint64_t idx = (e.addr - DUDE_PERSIST_MMAP_ADDR) / 64;
                if (idx < BITMAP_SIZE / 64)
                    __sync_fetch_and_and(&alloc_bitmap[idx / 64],
                                         ~(1ULL << (idx % 64)));
                break;
            }

            default:
                break;
            }

            t++;
        }

        std::atomic_thread_fence(std::memory_order_release);
        g_shared->tail.store(t, std::memory_order_release);

        // Periodic msync
        if (t - last_sync > 1000) {
            msync(local_persist, g_persist_size, MS_ASYNC);
            last_sync = t;
        }
    }

    msync(local_persist, g_persist_size, MS_SYNC);
    munmap(local_persist, g_persist_size);
    fprintf(stderr, "[DUDETM] replayer exiting, pid=%d\n", getpid());
    _exit(0);
}

// ── Init ───────────────────────────────────────────────────────
inline void
init(uint32_t sym_count, void** sym_addresses, uint64_t* sym_sizes)
{
    uint64_t data_size = 0;
    for (uint32_t i = 0; i < sym_count; i++)
        data_size += sym_sizes[i];

    size_t hdr_size = 24;
    g_heap_start_off = hdr_size + data_size;
    g_persist_size   = hdr_size + data_size + DUDE_PERSIST_HEAP_SIZE;

    // Persistent data mmap
    int pfd = open(DUDE_PERSIST_FILE, O_RDWR | O_CREAT, 0644);
    if (pfd < 0) { perror("dudetm: open persist"); exit(1); }

    struct stat pst;
    bool file_has_data = (fstat(pfd, &pst) == 0 &&
                          (size_t)pst.st_size >= g_persist_size);

    if (ftruncate(pfd, (off_t)g_persist_size) < 0) {
        perror("dudetm: ftruncate persist"); close(pfd); exit(1);
    }

    g_persist_base = (uint8_t*)mmap(
        (void*)DUDE_PERSIST_MMAP_ADDR, g_persist_size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, pfd, 0);
    if (g_persist_base == MAP_FAILED) {
        g_persist_base = (uint8_t*)mmap(
            nullptr, g_persist_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
        if (g_persist_base == MAP_FAILED) {
            perror("dudetm: mmap persist"); close(pfd); exit(1);
        }
        fprintf(stderr, "[DUDETM] WARNING: persist at %p (not fixed addr)\n",
                (void*)g_persist_base);
    } else {
        fprintf(stderr, "[DUDETM] persist at fixed address %p\n",
                (void*)g_persist_base);
    }
    close(pfd);

    uint64_t magic   = 0x4D544544554455ULL;
    uint32_t version = 1;

    if (file_has_data) {
        uint64_t fmagic;
        memcpy(&fmagic, g_persist_base, 8);
        uint32_t fver, fcount;
        memcpy(&fver, g_persist_base + 8, 4);
        memcpy(&fcount, g_persist_base + 12, 4);
        if (fmagic == magic && fver == version && fcount == sym_count) {
            uint64_t off = hdr_size;
            for (uint32_t i = 0; i < sym_count; i++) {
                uint64_t sz = sym_sizes[i];
                memcpy(sym_addresses[i], g_persist_base + off, sz);
                off += sz;
            }
            fprintf(stderr, "[DUDETM] restored %u symbols\n", sym_count);
        } else {
            fprintf(stderr, "[DUDETM] file format mismatch, reinitializing\n");
            file_has_data = false;
        }
    }

    if (!file_has_data) {
        memcpy(g_persist_base, &magic, 8);
        memcpy(g_persist_base + 8, &version, 4);
        memcpy(g_persist_base + 12, &sym_count, 4);
        uint64_t bump_zero = 0;
        memcpy(g_persist_base + 16, &bump_zero, 8);
        uint64_t off = hdr_size;
        for (uint32_t i = 0; i < sym_count; i++) {
            uint64_t sz = sym_sizes[i];
            memcpy(g_persist_base + off, sym_addresses[i], sz);
            off += sz;
        }
        msync(g_persist_base, g_persist_size, MS_SYNC);
        fprintf(stderr, "[DUDETM] initialized new persistent store\n");
    }

    // Build symbol range table
    g_sym_count = sym_count;
    g_sym_ranges = new DUDESymbolRange[sym_count];
    uint64_t off = hdr_size;
    for (uint32_t i = 0; i < sym_count; i++) {
        uintptr_t start = (uintptr_t)sym_addresses[i];
        uint64_t  sz    = sym_sizes[i];
        g_sym_ranges[i].addr_start = start;
        g_sym_ranges[i].addr_end   = start + sz;
        g_sym_ranges[i].file_off   = off;
        off += sz;
    }

    // Circular buffer mmap
    size_t rl_size = sizeof(DUDEShared);
    int rfd = open(DUDE_REDOLOG_FILE, O_RDWR | O_CREAT, 0644);
    if (rfd < 0) { perror("dudetm: open redolog"); exit(1); }
    if (ftruncate(rfd, (off_t)rl_size) < 0) {
        perror("dudetm: ftruncate redolog"); close(rfd); exit(1);
    }
    g_shared = (DUDEShared*)mmap(nullptr, rl_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, rfd, 0);
    if (g_shared == MAP_FAILED) {
        perror("dudetm: mmap redolog"); close(rfd); exit(1);
    }
    close(rfd);

    // Signal handler
    struct sigaction sa;
    sa.sa_handler = dudetm_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);

    // Fork replayer
    g_replayer_pid = fork();
    if (g_replayer_pid < 0) {
        perror("dudetm: fork"); exit(1);
    }

    if (g_replayer_pid == 0) {
        replayer_loop();
        _exit(0);
    }

    fprintf(stderr, "[DUDETM] replayer forked, pid=%d\n", g_replayer_pid);
}

// ── Shutdown ───────────────────────────────────────────────────
inline void
shutdown()
{
    if (g_replayer_pid > 0) {
        g_shutdown = true;
        int status;
        waitpid(g_replayer_pid, &status, 0);
        g_replayer_pid = 0;
    }
    if (g_persist_base) {
        msync(g_persist_base, g_persist_size, MS_SYNC);
    }
    if (g_shared) {
        munmap(g_shared, sizeof(DUDEShared));
        g_shared = nullptr;
    }
    if (g_persist_base) {
        munmap(g_persist_base, g_persist_size);
        g_persist_base = nullptr;
    }
    delete[] g_sym_ranges;
    g_sym_ranges = nullptr;
    g_sym_count  = 0;
    fprintf(stderr, "[DUDETM] shutdown complete\n");
}

} // namespace dudetm
