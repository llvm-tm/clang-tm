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

#include "tm_common.hpp"

namespace dudetm
{

// ── Entry op-types ─────────────────────────────────────────────
enum EntryOpType : uint8_t {
    OP_WRITE        = 0,
    OP_COMMIT_BEGIN = 1,
    OP_MALLOC       = 2,
    OP_FREE         = 3,
};

// ── Redo Log Entry (32 bytes) ──────────────────────────────────
struct DUDERedoEntry {
    uint8_t          op_type;
    uint8_t          _pad[7];
    uint64_t         addr;
    stm::any_type_t  val;
    stm::ValueType   type;
};

// ── Per-thread circular buffer (in shared mmap) ────────────────
constexpr size_t RING_LOG_SIZE   = 1UL << 16;   // 65,536 entries (2 MB per thread)
constexpr size_t RING_LOG_MASK   = RING_LOG_SIZE - 1;
constexpr uint64_t MAX_PENDING   = RING_LOG_SIZE / 2;

struct PerThreadLog {
    std::atomic<uint64_t> head;   // producer advances after filling
    std::atomic<uint64_t> tail;   // consumer advances after replay
    uint8_t               _pad[48];
    DUDERedoEntry         entries[RING_LOG_SIZE];
};
static_assert(sizeof(PerThreadLog) % 64 == 0, "cache-line aligned");

// ── Shared control block (inter-process via MAP_SHARED) ────────
struct SharedControl {
    std::atomic<uint64_t> global_commit_seq{0};
    std::atomic<int>      next_log_idx{0};
    std::atomic<bool>     shutdown{false};
    std::atomic<bool>     replayer_ready{false};
};

// ── Persistent storage ─────────────────────────────────────────
constexpr size_t PERSIST_HEAP_SIZE = 64UL * 1024 * 1024;
static constexpr uintptr_t PERSIST_MMAP_ADDR = 0x600000000000ULL;
static constexpr const char* PERSIST_FILE = "benchmark_results/dudetm_persist.bin";

// ── Global pointers (in shared mmap, survive fork) ──────────────
constexpr size_t MAX_THREADS = 32;

static SharedControl*   g_ctrl          = nullptr;  // mmap'd shared
static PerThreadLog*    g_logs          = nullptr;   // mmap'd shared
static uint8_t*          g_persist_base  = nullptr;   // mmap'd file-backed
static size_t            g_persist_size  = 0;
static size_t            g_heap_start_off = 0;
static pid_t      g_replayer_pid  = 0;

// Thread-local log index (process-local, not shared)
static thread_local int tls_log_idx = -1;

// ── Symbol tracking ────────────────────────────────────────────
struct DUDESymbolRange {
    uintptr_t addr_start;
    uintptr_t addr_end;
    size_t    file_off;
};
static DUDESymbolRange* g_sym_ranges = nullptr;  // small, process-local OK
static uint32_t         g_sym_count  = 0;

// ── Signal handler ─────────────────────────────────────────────
static void dudetm_signal_handler(int) {
    if (g_ctrl) g_ctrl->shutdown.store(true, std::memory_order_relaxed);
}

// ── Get thread's log (assigns index on first call) ─────────────
inline PerThreadLog* get_thread_log() {
    if (tls_log_idx < 0) {
        tls_log_idx = g_ctrl->next_log_idx.fetch_add(1, std::memory_order_relaxed);
    }
    return &g_logs[tls_log_idx];
}

// ── Batch publish to per-thread log (lock-free) ────────────────
inline void
publish_batch(const DUDERedoEntry* entries, size_t count)
{
    if (count == 0) return;
    PerThreadLog* log = get_thread_log();

    uint64_t h = log->head.load(std::memory_order_relaxed);
    uint64_t t = log->tail.load(std::memory_order_acquire);

    while (h - t + count > MAX_PENDING) {
        t = log->tail.load(std::memory_order_acquire);
    }

    for (size_t i = 0; i < count; i++)
        log->entries[(h + i) & RING_LOG_MASK] = entries[i];

    std::atomic_thread_fence(std::memory_order_release);
    log->head.store(h + count, std::memory_order_release);
}

// ── Address → file offset ─────────────────────────────────────
// Checks both registered symbol ranges and the persistent mmap's heap area
// (so that writes to heap-allocated data are also persisted).
static inline size_t
addr_to_file_off(uintptr_t addr)
{
    // 1. Check registered symbol ranges (global TM variables)
    if (g_sym_count > 0) {
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
    }
    // 2. Check heap area within the persistent mmap
    //    (addresses returned by persistent tm_malloc bump allocator).
    //    The bump pointer is stored at offset 16 of the persistent file header
    //    (sizeof(magic)=8 + sizeof(version)=4 + sizeof(sym_count)=4), atomically
    //    updated by the parent's tm_malloc and readable by the replayer via MAP_SHARED.
    if (g_persist_base) {
        uintptr_t base   = (uintptr_t)g_persist_base;
        uintptr_t hstart = base + g_heap_start_off;
        uint64_t  bump   = *reinterpret_cast<const uint64_t*>(g_persist_base + 16);
        if (addr >= hstart && addr < hstart + bump)
            return addr - base;  // direct file offset
    }
    return (size_t)-1;
}

// ── Replayer-local bump allocator state ──────────────────────
// (only accessed by the replayer child process)
static size_t replayer_heap_bump = 0;

// ── Replay a single op ────────────────────────────────────────
inline void replay_op(const DUDERedoEntry& e, uint8_t* local_persist)
{
    switch (e.op_type) {
    case OP_WRITE: {
        size_t off = addr_to_file_off(e.addr);
        stm::write_value_to_addr(
            reinterpret_cast<void*>(e.addr), e.val, e.type);
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
        // Track the allocation so addr_to_file_off knows the heap range.
        // The parent atomically increments the header bump via
        // __atomic_fetch_add(g_persist_base + 16, ...), and the replayer
        // reads the value from the MAP_SHARED header for its own tracking.
        size_t sz = e.val.u8;
        replayer_heap_bump += sz;
        break;
    }
    case OP_FREE:
        // Bump allocator: free is a no-op (space is never reclaimed).
        break;
    default:
        break;
    }
}

// ── Replayer loop: k-way merge across per-thread logs ────────
inline void
replayer_loop()
{
    int fd = open(PERSIST_FILE, O_RDWR, 0644);
    if (fd < 0) { perror("dudetm replayer: open persist"); _exit(1); }

    uint8_t* local_persist = (uint8_t*)mmap(
        (void*)PERSIST_MMAP_ADDR, g_persist_size,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (local_persist == MAP_FAILED) {
        perror("dudetm replayer: mmap persist");
        close(fd); _exit(1);
    }
    close(fd);

    // Initialize the replayer-local bump from the persistent file header.
    // The parent may have advanced it since the file was first created.
    replayer_heap_bump = *reinterpret_cast<const uint64_t*>(local_persist + 16);

    // Signal parent that replayer is ready (shared memory write visible to parent)
    g_ctrl->replayer_ready.store(true, std::memory_order_release);

    fprintf(stderr, "[DUDETM] replayer started, pid=%d\n", getpid());
    fflush(stderr);

    // Cursor state per log (process-local, only accessed by replayer)
    struct Cursor {
        uint64_t tail;
        uint64_t next_seq;
        bool     has_pending;
    };
    Cursor cursors[MAX_THREADS] = {};

    uint64_t last_sync = 0;
    uint64_t total_replayed = 0;

    while (!g_ctrl->shutdown.load(std::memory_order_relaxed)) {
        // Refresh heads and find smallest pending seq
        int best_log = -1;
        uint64_t best_seq = UINT64_MAX;

        for (size_t i = 0; i < MAX_THREADS; i++) {
            auto* slog = &g_logs[i];
            auto& c = cursors[i];
            uint64_t h = slog->head.load(std::memory_order_acquire);

            while (!c.has_pending && c.tail < h) {
                DUDERedoEntry e = slog->entries[c.tail & RING_LOG_MASK];
                if (e.op_type == OP_COMMIT_BEGIN) {
                    c.next_seq = e.addr;
                    c.has_pending = true;
                    c.tail++;
                    break;
                }
                c.tail++;
            }

            if (c.has_pending && c.next_seq < best_seq) {
                best_seq = c.next_seq;
                best_log = (int)i;
            }
        }

        if (best_log < 0) {
            usleep(100);
            continue;
        }

        // Replay all entries from best_log until next COMMIT_BEGIN
        auto* slog = &g_logs[best_log];
        auto& c = cursors[best_log];
        uint64_t h = slog->head.load(std::memory_order_acquire);

        while (c.tail < h && !g_ctrl->shutdown.load(std::memory_order_relaxed)) {
            DUDERedoEntry e = slog->entries[c.tail & RING_LOG_MASK];
            if (e.op_type == OP_COMMIT_BEGIN) break;
            replay_op(e, local_persist);
            total_replayed++;
            c.tail++;

            if ((c.tail & 0xFF) == 0)
                h = slog->head.load(std::memory_order_acquire);
        }

        c.has_pending = false;
        slog->tail.store(c.tail, std::memory_order_release);

        if (total_replayed - last_sync > 1000) {
            msync(local_persist, g_persist_size, MS_ASYNC);
            last_sync = total_replayed;
        }
    }

    // ── Final drain: process any remaining entries ──────────────
    for (size_t i = 0; i < MAX_THREADS; i++) {
        auto* slog = &g_logs[i];
        auto& c = cursors[i];
        uint64_t h = slog->head.load(std::memory_order_acquire);

        // Scan for pending commits
        while (!c.has_pending && c.tail < h) {
            DUDERedoEntry e = slog->entries[c.tail & RING_LOG_MASK];
            if (e.op_type == OP_COMMIT_BEGIN) {
                c.has_pending = true;
                c.tail++;
                break;
            }
            c.tail++;
        }

        // Replay remaining entries
        while (c.tail < h) {
            DUDERedoEntry e = slog->entries[c.tail & RING_LOG_MASK];
            if (e.op_type == OP_COMMIT_BEGIN) {
                c.tail++;  // skip marker
                continue;
            }
            replay_op(e, local_persist);
            total_replayed++;
            c.tail++;
            if ((c.tail & 0xFF) == 0)
                h = slog->head.load(std::memory_order_acquire);
        }

        slog->tail.store(c.tail, std::memory_order_release);
    }

    msync(local_persist, g_persist_size, MS_SYNC);
    munmap(local_persist, g_persist_size);
    fprintf(stderr, "[DUDETM] replayer exiting, pid=%d, ops=%llu\n",
            getpid(), (unsigned long long)total_replayed);
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
    g_persist_size   = hdr_size + data_size + PERSIST_HEAP_SIZE;

    // ── Persistent data mmap (file-backed) ──────────────────────
    int pfd = open(PERSIST_FILE, O_RDWR | O_CREAT, 0644);
    if (pfd < 0) { perror("dudetm: open persist"); exit(1); }

    struct stat pst;
    bool file_has_data = (fstat(pfd, &pst) == 0 &&
                          (size_t)pst.st_size >= g_persist_size);

    if (ftruncate(pfd, (off_t)g_persist_size) < 0) {
        perror("dudetm: ftruncate persist"); close(pfd); exit(1);
    }

    g_persist_base = (uint8_t*)mmap(
        (void*)PERSIST_MMAP_ADDR, g_persist_size,
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

    // ── Build symbol range table (process-local) ─────────────
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
    // Sort by address (required for binary search in addr_to_file_off)
    for (uint32_t i = 1; i < sym_count; i++) {
        DUDESymbolRange key = g_sym_ranges[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && g_sym_ranges[j].addr_start > key.addr_start) {
            g_sym_ranges[j + 1] = g_sym_ranges[j];
            j--;
        }
        g_sym_ranges[j + 1] = key;
    }

    // ── Shared memory allocations (BEFORE fork) ──────────────
    size_t logs_size = MAX_THREADS * sizeof(PerThreadLog);
    g_logs = (PerThreadLog*)mmap(nullptr, logs_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_logs == MAP_FAILED) { perror("dudetm: mmap logs"); exit(1); }
    // Zero-initialize all logs (head=0, tail=0, entries zeroed)
    memset(g_logs, 0, logs_size);

    g_ctrl = (SharedControl*)mmap(nullptr, 4096,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_ctrl == MAP_FAILED) { perror("dudetm: mmap ctrl"); exit(1); }
    new (g_ctrl) SharedControl();  // placement-new for atomic init

    // ── Signal handler ───────────────────────────────────────
    struct sigaction sa;
    sa.sa_handler = dudetm_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);

    // ── Fork replayer ────────────────────────────────────────
    g_replayer_pid = fork();
    if (g_replayer_pid < 0) {
        perror("dudetm: fork"); exit(1);
    }

    if (g_replayer_pid == 0) {
        replayer_loop();
        _exit(0);
    }

    // Parent: wait for replayer to signal readiness via shared memory
    while (!g_ctrl->replayer_ready.load(std::memory_order_acquire))
        usleep(100);

    fprintf(stderr, "[DUDETM] replayer forked, pid=%d\n", g_replayer_pid);
}

// ── Shutdown ───────────────────────────────────────────────────
inline void
shutdown()
{
    if (g_replayer_pid > 0) {
        // Signal shutdown via shared memory (visible to child)
        g_ctrl->shutdown.store(true, std::memory_order_release);
        int status;
        waitpid(g_replayer_pid, &status, 0);
        g_replayer_pid = 0;
    }
    if (g_persist_base) {
        msync(g_persist_base, g_persist_size, MS_SYNC);
    }
    if (g_logs) {
        munmap(g_logs, MAX_THREADS * sizeof(PerThreadLog));
        g_logs = nullptr;
    }
    if (g_ctrl) {
        munmap(g_ctrl, 4096);
        g_ctrl = nullptr;
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
