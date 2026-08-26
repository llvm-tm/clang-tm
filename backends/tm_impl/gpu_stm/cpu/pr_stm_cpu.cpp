// ── PR-STM CPU Fallback ────────────────────────────────────────────
// Emulates PR-STM on CPU using std::thread to simulate GPU lanes
// within a warp. Each warp is a group of threads that execute in
// lockstep (phase barriers enforce SIMT semantics).
//
// Usage:
//   cpu_pr_stm_emulate(num_warps, warp_size, tx_func, arg);
//   - Creates num_warps × warp_size threads
//   - Each thread executes tx_func(warp_id, lane_id, arg)
//   - Phase barriers between begin/read/write/validate/lock/commit
//   - Warp-level abort via shared atomic flag
//
// Portability to SYCL:
//   std::thread → sycl::parallel_for(nd_range)
//   std::barrier → sycl::group_barrier
//   std::atomic → sycl::atomic_ref
//   Per-thread arrays → sycl::local_accessor

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>

#include "gpu_stm_api.h"

// ── Lock table (shared across all warps/threads) ──────────────────

static uint32_t *cpu_lock_table = nullptr;
static std::atomic<uint64_t> cpu_global_clock{0};
static uint32_t *cpu_data = nullptr;
static std::atomic<uint64_t> cpu_committed{0};
static std::atomic<uint64_t> cpu_aborted{0};

// ── Per-warp synchronization (C++17-compatible spin barrier) ──────
// Each warp has a barrier that all its lanes synchronize on.
// This emulates SIMT lockstep: all threads in a warp reach the
// same phase before any proceeds.

struct alignas(64) SpinBarrier {
    std::atomic<int> count;
    std::atomic<int> sense;
    int num;

    SpinBarrier(int n) : count(0), sense(0), num(n) {}

    void arrive_and_wait() {
        int my_sense = sense.load(std::memory_order_relaxed);
        if (count.fetch_add(1, std::memory_order_acq_rel) == num - 1) {
            count.store(0, std::memory_order_relaxed);
            sense.store(my_sense ^ 1, std::memory_order_release);
        } else {
            while (sense.load(std::memory_order_acquire) == my_sense)
                ;  // spin
        }
    }
};

struct WarpSync {
    SpinBarrier *barrier;
    std::atomic<int> warp_abort;
    volatile int phase;  // 0=idle, 1=read, 2=write, 3=validate, 4=lock, 5=commit

    WarpSync(int num_lanes)
        : barrier(new SpinBarrier(num_lanes))
        , warp_abort(0)
        , phase(0) {}

    ~WarpSync() { delete barrier; }
};

static std::vector<WarpSync *> warp_syncs;

// ── Per-thread state (one per lane) ────────────────────────────────

struct LaneState {
    int warp_id;
    int lane_id;
    uint8_t priority;
    uint32_t read_addrs[PR_STM_MAX_READS];
    uint32_t read_vers[PR_STM_MAX_READS];
    int num_reads;
    uint32_t write_addrs[PR_STM_MAX_WRITES];
    int num_writes;
    int write_vals[PR_STM_MAX_WRITES];
};

// ── Read-set helper: check if an address is in our write-set ──────

static int in_write_set(LaneState *ls, uint32_t addr) {
    for (int i = 0; i < ls->num_writes; i++) {
        if (ls->write_addrs[i] == addr) return 1;
    }
    return 0;
}

// ── PR-STM transaction body (called by each lane thread) ──────────

static void pr_stm_lane_thread(LaneState *ls, int num_addrs,
                                int reads_per_thread,
                                int writes_per_thread) {
    WarpSync *sync = warp_syncs[ls->warp_id];
    int warp = ls->warp_id;
    int lane = ls->lane_id;

    for (int tx_iter = 0; tx_iter < 10; tx_iter++) {
        // ── BEGIN ─────────────────────────────────────────────────
        uint64_t start_clock = cpu_global_clock.load(std::memory_order_acquire);
        ls->num_reads = 0;
        ls->num_writes = 0;
        sync->warp_abort.store(0, std::memory_order_relaxed);
        sync->phase = 1;

        // ── READ phase (SIMT lockstep via barrier) ───────────────
        for (int r = 0; r < reads_per_thread; r++) {
            int addr_idx = (lane * reads_per_thread + r) % num_addrs;
            uint32_t lock_word = __atomic_load_n(
                &cpu_lock_table[addr_idx], __ATOMIC_ACQUIRE);

            // Check lock not held by another warp
            if (pr_stm_is_locked(lock_word)) {
                sync->warp_abort.store(1, std::memory_order_relaxed);
            }

            // Record address + version
            ls->read_addrs[ls->num_reads] = (uint32_t)addr_idx;
            ls->read_vers[ls->num_reads]  = pr_stm_get_version(lock_word);
            ls->num_reads++;

            // Simulate data read
            volatile uint32_t val = cpu_data[addr_idx];
            (void)val;
        }
        sync->barrier->arrive_and_wait();  // SIMT barrier: all lanes done reading

        // Check warp-level abort
        if (sync->warp_abort.load(std::memory_order_acquire)) {
            goto abort_tx;
        }

        // ── WRITE phase (buffer) ─────────────────────────────────
        sync->phase = 2;
        for (int w = 0; w < writes_per_thread; w++) {
            int addr_idx = (lane * writes_per_thread + w + 1) % num_addrs;
            ls->write_addrs[ls->num_writes] = (uint32_t)addr_idx;
            ls->write_vals[ls->num_writes] = (int)(lane + warp * 32 + w);
            ls->num_writes++;
        }
        sync->barrier->arrive_and_wait();

        // ── VALIDATE phase ────────────────────────────────────────
        sync->phase = 3;
        { int local_valid = 1;
        for (int i = 0; i < ls->num_reads; i++) {
            // Skip validation if this read is also in our write-set (read-own-write)
            bool in_write_set = false;
            for (int w = 0; w < ls->num_writes; w++) {
                if (ls->write_addrs[w] == ls->read_addrs[i]) {
                    in_write_set = true;
                    break;
                }
            }
            if (in_write_set) continue;

            uint32_t lock_word = __atomic_load_n(
                &cpu_lock_table[ls->read_addrs[i]], __ATOMIC_ACQUIRE);
            if (pr_stm_get_version(lock_word) != ls->read_vers[i] ||
                pr_stm_is_locked(lock_word)) {
                local_valid = 0;
            }
        }
        // SIMT: if any lane invalid, all abort
        if (!local_valid) {
            sync->warp_abort.store(1, std::memory_order_relaxed);
        }
        sync->barrier->arrive_and_wait();
        if (sync->warp_abort.load(std::memory_order_acquire)) {
            goto abort_tx;
        }
        }

        // ── LOCK phase (priority-based) ──────────────────────────
        sync->phase = 4;
        // First, check if we can acquire all locks
        for (int w = 0; w < ls->num_writes; w++) {
            int ai = (int)ls->write_addrs[w];
            uint32_t old = __atomic_load_n(
                &cpu_lock_table[ai], __ATOMIC_ACQUIRE);

            if (pr_stm_is_locked(old)) {
                uint8_t holder = pr_stm_get_priority(old);
                // Can only acquire if we already hold it (same priority)
                // Higher priority holder = abort; lower = wait
                if (holder != ls->priority) {
                    sync->warp_abort.store(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        sync->barrier->arrive_and_wait();
        if (sync->warp_abort.load(std::memory_order_acquire)) {
            goto abort_tx;
        }

        // Acquire locks (atomic CAS)
        for (int w = 0; w < ls->num_writes && !sync->warp_abort.load(); w++) {
            int ai = (int)ls->write_addrs[w];
            uint32_t expected = __atomic_load_n(&cpu_lock_table[ai],
                __ATOMIC_ACQUIRE);
            if (!pr_stm_is_locked(expected)) {
                uint32_t desired = pr_stm_make_entry(
                    ls->priority, pr_stm_get_version(expected), 1);
                __atomic_compare_exchange_n(&cpu_lock_table[ai],
                    &expected, desired, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
            }
            // If already locked by us, skip (already hold it)
        }
        sync->barrier->arrive_and_wait();
        if (sync->warp_abort.load(std::memory_order_acquire)) {
            goto abort_tx;
        }

        // ── RE-VALIDATE after lock acquisition ────────────────────
        // Ensure no concurrent warp modified our read-set while
        // we were acquiring locks. Skip reads in our write-set.
        for (int i = 0; i < ls->num_reads; i++) {
            // Skip if read-own-write
            bool in_write_set = false;
            for (int w = 0; w < ls->num_writes; w++) {
                if (ls->write_addrs[w] == ls->read_addrs[i]) {
                    in_write_set = true;
                    break;
                }
            }
            if (in_write_set) continue;

            uint32_t lock_word = __atomic_load_n(
                &cpu_lock_table[ls->read_addrs[i]], __ATOMIC_ACQUIRE);
            if (pr_stm_is_locked(lock_word)) {
                uint8_t holder = pr_stm_get_priority(lock_word);
                if (holder != ls->priority) {
                    // Another warp holds the lock - abort
                    sync->warp_abort.store(1, std::memory_order_relaxed);
                    break;
                }
                // We hold the lock - version must still match
                if (pr_stm_get_version(lock_word) != ls->read_vers[i]) {
                    sync->warp_abort.store(1, std::memory_order_relaxed);
                    break;
                }
            } else {
                // Lock free - version must match
                if (pr_stm_get_version(lock_word) != ls->read_vers[i]) {
                    sync->warp_abort.store(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        sync->barrier->arrive_and_wait();
        if (sync->warp_abort.load(std::memory_order_acquire)) {
            // Release locks we acquired
            for (int w = 0; w < ls->num_writes; w++) {
                int ai = (int)ls->write_addrs[w];
                uint32_t lw = __atomic_load_n(&cpu_lock_table[ai],
                    __ATOMIC_RELAXED);
                if (pr_stm_is_locked(lw) && pr_stm_get_priority(lw) == ls->priority) {
                    uint32_t new_entry = pr_stm_make_entry(0, pr_stm_get_version(lw), 0);
                    __atomic_store_n(&cpu_lock_table[ai], new_entry, __ATOMIC_RELEASE);
                }
            }
            goto abort_tx;
        }

        // ── COMMIT phase ──────────────────────────────────────────
        { sync->phase = 5;

        // Thread fence (seq_cst)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Increment global clock (warp-level: only lane 0 does this)
        if (lane == 0) {
            cpu_global_clock.fetch_add(1, std::memory_order_acq_rel);
        }
        sync->barrier->arrive_and_wait();
        uint64_t commit_clock = cpu_global_clock.load(std::memory_order_acquire);

        // Write-back data values
        for (int w = 0; w < ls->num_writes; w++) {
            int ai = (int)ls->write_addrs[w];
            cpu_data[ai] = (uint32_t)ls->write_vals[w];
        }

        // Fence before releasing locks
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Release locks
        for (int w = 0; w < ls->num_writes; w++) {
            int ai = (int)ls->write_addrs[w];
            uint32_t new_entry = pr_stm_make_entry(0, (uint32_t)commit_clock, 0);
            __atomic_store_n(&cpu_lock_table[ai], new_entry, __ATOMIC_RELEASE);
        }

        // Count commit
        if (lane == 0) {
            cpu_committed.fetch_add(1, std::memory_order_relaxed);
        }

        // Done. Continue to next transaction.
        sync->barrier->arrive_and_wait();
        continue; }

    abort_tx:
        // ── ABORT ────────────────────────────────────────────────
        // Release any locks we may have acquired
        for (int w = 0; w < ls->num_writes; w++) {
            int ai = (int)ls->write_addrs[w];
            uint32_t lw = __atomic_load_n(&cpu_lock_table[ai],
                __ATOMIC_RELAXED);
            if (pr_stm_is_locked(lw) && pr_stm_get_priority(lw) == ls->priority) {
                uint32_t new_entry = pr_stm_make_entry(0, pr_stm_get_version(lw), 0);
                __atomic_store_n(&cpu_lock_table[ai], new_entry, __ATOMIC_RELEASE);
            }
        }
        if (lane == 0) {
            cpu_aborted.fetch_add(1, std::memory_order_relaxed);
        }
        sync->barrier->arrive_and_wait();
    }
}

// ── CPU emulation entry point ──────────────────────────────────────

void cpu_pr_stm_emulate(int num_warps, int warp_size,
                         int num_addrs, int reads_per_thread,
                         int writes_per_thread) {
    // Initialize shared state
    cpu_lock_table = (uint32_t *)calloc(PR_STM_LOCKTABLE_SIZE, sizeof(uint32_t));
    cpu_data = (uint32_t *)calloc(num_addrs, sizeof(uint32_t));
    cpu_global_clock.store(0, std::memory_order_relaxed);
    cpu_committed.store(0, std::memory_order_relaxed);
    cpu_aborted.store(0, std::memory_order_relaxed);

    // Create warp sync objects
    warp_syncs.clear();
    for (int w = 0; w < num_warps; w++) {
        warp_syncs.push_back(new WarpSync(warp_size));
    }

    // Create lanes: num_warps * warp_size threads
    std::vector<std::thread> threads;
    std::vector<LaneState> states(num_warps * warp_size);

    for (int w = 0; w < num_warps; w++) {
        for (int l = 0; l < warp_size; l++) {
            int idx = w * warp_size + l;
            states[idx].warp_id = w;
            states[idx].lane_id = l;
            states[idx].priority = (uint8_t)(w + 1);
            states[idx].num_reads = 0;
            states[idx].num_writes = 0;
        }
    }

    // Launch threads
    for (int w = 0; w < num_warps; w++) {
        for (int l = 0; l < warp_size; l++) {
            int idx = w * warp_size + l;
            threads.emplace_back(pr_stm_lane_thread, &states[idx],
                                  num_addrs, reads_per_thread,
                                  writes_per_thread);
        }
    }

    // Wait for all threads to complete
    for (auto &t : threads) {
        t.join();
    }

    // Cleanup
    for (auto *ws : warp_syncs) {
        delete ws;
    }
    warp_syncs.clear();
    free(cpu_lock_table);
    free(cpu_data);

    printf("[CPU PR-STM] %lu commits, %lu aborts\n",
           cpu_committed.load(), cpu_aborted.load());
}
