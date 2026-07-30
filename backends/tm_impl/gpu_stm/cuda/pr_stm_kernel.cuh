#pragma once

#include "tm_gpu_platform.hpp"
#include <cstdint>

// ── Lock table (device-side) ──────────────────────────────────────
// Global memory array of 32-bit lock words.
// One entry per address (simplified; real impl uses hash table).

__device__ __forceinline__
uint32_t pr_stm_load_lock(uint32_t *lock_table, int idx) {
    return __ldg(&lock_table[idx]);
}

__device__ __forceinline__
uint32_t pr_stm_atomic_acquire(uint32_t *lock_table, int idx,
                                uint32_t expected, uint32_t desired) {
    return atomicCAS(&lock_table[idx], expected, desired);
}

// ── PR-STM transaction (one warp) ─────────────────────────────────
//
// Launched as: pr_stm_kernel<<<num_warps, WARP_SIZE>>>(...)
// Each block is one warp. blockDim.x must equal WARP_SIZE (32).
//
// Shared memory layout:
//   [0 .. WARP_SIZE*MAX_READS-1]       : read-set addresses (per-lane)
//   [WARP_SIZE*MAX_READS .. +MAX_READS]: read-set versions (per-lane)
//   [...]                               : write-set addresses
//
// Portability note (for SYCL / HIP):
//   CUDA        → SYCL (AdaptiveCpp)   → HIP
//   threadIdx.x → item.get_local_id()  → hipThreadIdx_x
//   blockDim.x  → sg.get_local_range() → blockDim.x
//   __ballot_sync → sg.ballot()        → __ballot_sync
//   atomicCAS   → atomic_ref::compare_exchange_strong → atomicCAS
//   __syncthreads → group_barrier      → __syncthreads
//   __threadfence → atomic_fence(seq_cst) → __threadfence

template <int MAX_READS, int MAX_WRITES>
__global__ void pr_stm_kernel(
    uint32_t *lock_table,       // [LOCKTABLE_SIZE] in global memory
    uint64_t *global_clock,     // single uint64_t (monotonic counter)
    uint32_t *data,             // transactional data array
    int       num_addrs,        // size of data array
    uint64_t *committed_count,  // output: total successful commits
    uint64_t *abort_count,      // output: total aborts
    int       reads_per_thread, // how many addresses each lane reads
    int       writes_per_thread // how many addresses each lane writes
) {
    // ── Lane and warp identification ──────────────────────────────
    int lane = threadIdx.x;             // thread within warp (0..WARP_SIZE-1)
    int warp = blockIdx.x;              // warp ID (0..num_warps-1)
    int num_warps = gridDim.x;

    // ── Shared memory (read-set / write-set per lane) ─────────────
    __shared__ uint32_t s_read_addr[MAX_READS * 32];   // addresses read
    __shared__ uint32_t s_read_ver[MAX_READS * 32];    // observed versions
    __shared__ int      s_read_cnt;                    // total reads done this round
    __shared__ int      s_write_cnt;                   // total writes done this round
    __shared__ volatile int s_warp_abort;              // warp-level abort flag

    // Per-lane pointers into shared memory
    uint32_t *my_read_addr = &s_read_addr[lane * MAX_READS];
    uint32_t *my_read_ver  = &s_read_ver[lane * MAX_READS];

    // Persistent kernel: each warp repeatedly executes transactions
    while (true) {
        // ── Phase 1: BEGIN ───────────────────────────────────────
        // Snapshot global clock. All lanes read the same value.
        uint64_t start_clock = __ldg(global_clock);

        if (lane == 0) {
            s_read_cnt = 0;
            s_write_cnt = 0;
            s_warp_abort = 0;
        }
        __syncwarp();  // ensure initializers visible to all lanes

        // ── Phase 2: READ ────────────────────────────────────────
        // Each lane reads `reads_per_thread` addresses.
        // Addresses are deterministic (based on lane + warp + round).
        // In a real application, the transaction body would determine
        // which addresses to read.
        int my_reads = 0;
        for (int r = 0; r < reads_per_thread; r++) {
            // Deterministic address selection (PR-STM benchmark style)
            int addr_idx = (lane * reads_per_thread + r) % num_addrs;
            uint32_t lock_word = pr_stm_load_lock(lock_table, addr_idx);

            // Check lock not held
            if (pr_stm_is_locked(lock_word)) {
                s_warp_abort = 1;
            }

            // Record address + version
            my_read_addr[my_reads] = (uint32_t)addr_idx;
            my_read_ver[my_reads]  = pr_stm_get_version(lock_word);
            my_reads++;

            // Simulate read: load data value (for side effects)
            volatile uint32_t val = __ldg(&data[addr_idx]);
            (void)val;
        }
        __syncwarp();

        if (lane == 0) {
            s_read_cnt += my_reads;
            // Warp-level any-of: check if any lane detected locked addr
            uint64_t mask = __ballot_sync(~0ULL, s_warp_abort != 0);
            if (mask) s_warp_abort = 1;
        }
        if (s_warp_abort) goto abort_tx;
        __syncwarp();

        // ── Phase 3..6 (wrapped to avoid goto-bypass over inits) ──
        {
        // Phase 3: WRITE (buffer)
        int my_writes = 0;
        for (int w = 0; w < writes_per_thread; w++) {
            int addr_idx = (lane * writes_per_thread + w + 1) % num_addrs;
            my_writes++;
        }
        __syncwarp();

        if (lane == 0) s_write_cnt += my_writes;

        // Phase 4: VALIDATE
        int local_valid = 1;
        for (int i = 0; i < my_reads; i++) {
            uint32_t lock_word = pr_stm_load_lock(lock_table, my_read_addr[i]);
            if (pr_stm_get_version(lock_word) != my_read_ver[i] ||
                pr_stm_is_locked(lock_word)) {
                local_valid = 0;
            }
        }

        {
        uint64_t abort_mask = __ballot_sync(~0ULL, local_valid == 0);
        if (abort_mask) {
            s_warp_abort = 1;
            goto abort_tx;
        }
        __syncwarp();
        }

        // Phase 5: LOCK
        uint8_t my_priority = (uint8_t)(warp + 1);

        for (int w = 0; w < my_writes; w++) {
            int addr_idx = w;
            uint32_t old = pr_stm_load_lock(lock_table, addr_idx);

            if (pr_stm_is_locked(old)) {
                uint8_t holder_prio = pr_stm_get_priority(old);
                if (holder_prio > my_priority) {
                    s_warp_abort = 1;
                    break;
                }
            }
        }

        {
        uint64_t abort_mask = __ballot_sync(~0ULL, s_warp_abort != 0);
        if (abort_mask) goto abort_tx;
        }

        // Acquire all write-set locks (atomicCAS)
        for (int w = 0; w < my_writes; w++) {
            int addr_idx = w;
            uint32_t expected = pr_stm_load_lock(lock_table, addr_idx);
            if (!pr_stm_is_locked(expected) ||
                pr_stm_get_priority(expected) <= my_priority) {
                uint32_t desired = pr_stm_make_entry(my_priority,
                                    pr_stm_get_version(expected), 1);
                uint32_t result = pr_stm_atomic_acquire(
                    lock_table, addr_idx, expected, desired);
                if (result != expected) {
                    s_warp_abort = 1;
                }
            }
        }

        __syncwarp();
        {
        uint64_t abort_mask = __ballot_sync(~0ULL, s_warp_abort != 0);
        if (abort_mask) goto abort_tx;
        }

        // Phase 6: COMMIT
        __threadfence();

        if (lane == 0) {
            atomicAdd((unsigned long long *)global_clock, 1ULL);
        }
        __syncwarp();
        uint64_t commit_clock = *global_clock;

        for (int w = 0; w < my_writes; w++) {
            int addr_idx = w;
            data[addr_idx] = (uint32_t)(lane + warp * 32 + w);
        }

        __threadfence();
        for (int w = 0; w < my_writes; w++) {
            int addr_idx = w;
            uint32_t new_entry = pr_stm_make_entry(0, commit_clock, 0);
            lock_table[addr_idx] = new_entry;
        }

        if (lane == 0) {
            atomicAdd((unsigned long long *)committed_count, 1ULL);
        }

        return;
        }

    abort_tx:
        if (lane == 0) {
            atomicAdd((unsigned long long *)abort_count, 1ULL);
        }
        return;
    }
}
