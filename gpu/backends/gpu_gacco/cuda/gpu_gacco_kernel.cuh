#pragma once
#include "tm_gpu_platform.hpp"
#include "gpu_gacco_api.h"
#include <cstdint>

// ── GAccO GPU kernel ─────────────────────────────────────────────
//
// Sorted-access lock ordering: all lanes acquire locks in address-
// sorted order to prevent deadlocks.  Locks are 32-bit words:
//   [holder_id:24 | version:7 | locked:1]
//
// Phases:
//   1. READ:   each lane reads addresses, tracks versions
//   2. SORT:   all lanes cooperate to sort write-set addresses
//   3. LOCK:   acquire locks in sorted address order
//   4. VALIDATE: re-check read-set versions
//   5. COMMIT:  write-back, release locks, increment clock
//
// This is a single-pass kernel (not persistent).  Caller launches
// one block per warp; blockDim.x must equal WARP_SIZE.

template <int MAX_READS, int MAX_WRITES>
__global__ void gpu_gacco_kernel(
    uint32_t *lock_table,     // [LOCKTABLE_SIZE] in global
    uint64_t *global_clock,   // monotonic counter
    uint32_t *data,           // transactional data array
    int       num_addrs,
    uint64_t *committed_count,
    uint64_t *abort_count,
    int       reads_per_thread,
    int       writes_per_thread
) {
    int lane = threadIdx.x;
    int warp = blockIdx.x;
    int num_warps = gridDim.x;

    // Shared memory layout
    extern __shared__ uint32_t s_buf[];
    // Layout: [read_addrs: MAX_READS*WARP_SIZE][read_vers: MAX_READS*WARP_SIZE]
    uint32_t *s_read_addr = s_buf;
    uint32_t *s_read_ver  = s_buf + MAX_READS * 32;
    // Write-set addresses + versions for sorting
    uint32_t *s_write_addr_tmp = s_buf + 2 * MAX_READS * 32;

    volatile int *s_abort = (volatile int*)(s_buf + 2 * MAX_READS * 32 + MAX_WRITES * 32);

    uint32_t *my_read_addr = &s_read_addr[lane * MAX_READS];
    uint32_t *my_read_ver  = &s_read_ver[lane * MAX_READS];
    uint32_t *my_write_tmp = &s_write_addr_tmp[lane * MAX_WRITES];

    if (lane == 0) *s_abort = 0;
    __syncwarp();

    // ── Phase 1: READ ──────────────────────────────────────────
    uint64_t start_clock = *global_clock;
    int my_reads = 0;
    for (int r = 0; r < reads_per_thread; r++) {
        int idx = (lane * reads_per_thread + r) % num_addrs;
        uint32_t lw = lock_table[idx];
        if (lw & GPU_GACCO_LOCKED_BIT) *s_abort = 1;
        my_read_addr[my_reads] = (uint32_t)idx;
        my_read_ver[my_reads]  = lw;
        my_reads++;
        volatile uint32_t v = data[idx];
        (void)v;
    }
    __syncwarp();

    int local_abort = *s_abort;
    uint64_t m = __ballot_sync(~0ULL, local_abort != 0);
    if (lane == 0 && m) *s_abort = 1;
    __syncwarp();
    if (*s_abort) {
        if (lane == 0) atomicAdd((unsigned long long*)abort_count, 1ULL);
        return;
    }

    // ── Phase 2: WRITE buffer ──────────────────────────────────
    int my_writes = 0;
    for (int w = 0; w < writes_per_thread; w++) {
        int idx = (lane * writes_per_thread + w + 1) % num_addrs;
        my_write_tmp[my_writes] = (uint32_t)idx;
        my_writes++;
    }

    // ── Phase 3: SORT write-set (even-odd transposition sort) ──
    // Each lane sorts its own write entries by address.
    for (int i = 0; i < my_writes; i++) {
        for (int j = 0; j < my_writes - 1; j++) {
            if (my_write_tmp[j] > my_write_tmp[j + 1]) {
                uint32_t t = my_write_tmp[j];
                my_write_tmp[j] = my_write_tmp[j + 1];
                my_write_tmp[j + 1] = t;
            }
        }
    }

    // ── Phase 4: LOCK (sorted order) ───────────────────────────
    // Each lane acquires locks for its write-set in sorted address order.
    // If any lock is already held, the warp aborts (no stealing).
    __syncwarp();

    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_tmp[w];
        uint32_t zero = 0;
        uint32_t desired = ((uint32_t)(warp + 1) << GPU_GACCO_PRIORITY_SHIFT)
                           | GPU_GACCO_LOCKED_BIT;
        uint32_t old = atomicCAS(&lock_table[addr_idx], zero, desired);
        if (old != 0) {
            *s_abort = 1;
        }
    }

    __syncwarp();
    local_abort = *s_abort;
    m = __ballot_sync(~0ULL, local_abort != 0);
    if (lane == 0 && m) *s_abort = 1;

    if (*s_abort) {
        // Release any locks we acquired
        for (int w = 0; w < my_writes; w++) {
            int addr_idx = (int)my_write_tmp[w];
            atomicCAS(&lock_table[addr_idx],
                      ((uint32_t)(warp + 1) << GPU_GACCO_PRIORITY_SHIFT) | GPU_GACCO_LOCKED_BIT,
                      0u);
        }
        if (lane == 0) atomicAdd((unsigned long long*)abort_count, 1ULL);
        return;
    }
    __syncwarp();

    // ── Phase 5: VALIDATE ──────────────────────────────────────
    int valid = 1;
    for (int i = 0; i < my_reads; i++) {
        uint32_t lw = lock_table[my_read_addr[i]];
        if (lw != my_read_ver[i]) { valid = 0; }
    }

    m = __ballot_sync(~0ULL, valid == 0);
    if (m) {
        // Release all locks
        for (int w = 0; w < my_writes; w++) {
            int addr_idx = (int)my_write_tmp[w];
            atomicCAS(&lock_table[addr_idx],
                      ((uint32_t)(warp + 1) << GPU_GACCO_PRIORITY_SHIFT) | GPU_GACCO_LOCKED_BIT,
                      0u);
        }
        if (lane == 0) atomicAdd((unsigned long long*)abort_count, 1ULL);
        return;
    }

    // ── Phase 6: COMMIT ────────────────────────────────────────
    __threadfence();
    if (lane == 0) atomicAdd((unsigned long long*)global_clock, 1ULL);
    __syncwarp();
    uint64_t commit_clock = *global_clock;

    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_tmp[w];
        data[addr_idx] = (uint32_t)(lane + warp * 32 + w);
    }
    __threadfence();

    // Release locks and update version
    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_tmp[w];
        uint32_t new_entry = (uint32_t)(commit_clock & ((1u << GPU_GACCO_PRIORITY_SHIFT) - 1));
        lock_table[addr_idx] = new_entry;
    }

    if (lane == 0) atomicAdd((unsigned long long*)committed_count, 1ULL);
}
