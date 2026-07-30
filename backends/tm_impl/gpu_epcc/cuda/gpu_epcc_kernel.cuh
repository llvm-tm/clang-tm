#pragma once
#include "tm_gpu_platform.hpp"
#include "gpu_epcc_api.h"
#include <cstdint>

// ── EPCC GPU kernel ─────────────────────────────────────────────
//
// Dynamic-priority STM: each warp has a retry-based priority.  When
// two warps contend for a write lock, the higher priority wins and
// the lower-priority warp aborts.  Priority increases with retries
// (age-based escalation) to prevent starvation.
//
// Lock word: [priority:24 | version:7 | locked:1]
//
// Phases:
//   1. READ:   each lane reads addresses, records versions
//   2. VALIDATE: re-check read-set versions against lock table
//   3. LOCK:   acquire write locks with priority-based conflict
//   4. COMMIT: write-back, release locks, increment global clock
//
// Priority = (abort_count << 8) | (warp + 1)
//   - Higher abort_count = higher priority (age-based escalation)
//   - warp ID breaks ties

// Per-warp abort counter (in device memory, indexed by warp)
__device__ uint32_t gpu_epcc_abort_counter[1024];

template <int MAX_READS, int MAX_WRITES>
__global__ void gpu_epcc_kernel(
    uint32_t *lock_table,
    uint64_t *global_clock,
    uint32_t *data,
    int       num_addrs,
    uint64_t *committed_count,
    uint64_t *abort_count,
    int       reads_per_thread,
    int       writes_per_thread
) {
    int lane = threadIdx.x;
    int warp = blockIdx.x;

    extern __shared__ uint32_t s_buf[];
    uint32_t *s_read_addr = s_buf;
    uint32_t *s_read_ver  = s_buf + MAX_READS * 32;
    uint32_t *s_write_addr = s_buf + 2 * MAX_READS * 32;

    volatile int *s_abort = (volatile int*)(s_buf + 2 * MAX_READS * 32 + MAX_WRITES * 32);

    uint32_t *my_read_addr = &s_read_addr[lane * MAX_READS];
    uint32_t *my_read_ver  = &s_read_ver[lane * MAX_READS];
    uint32_t *my_write_addr = &s_write_addr[lane * MAX_WRITES];

    if (lane == 0) *s_abort = 0;
    __syncwarp();

    uint32_t my_retries = (warp < 1024) ? gpu_epcc_abort_counter[warp] : 0;
    uint32_t my_priority = (my_retries << 8) | (uint32_t)(warp + 1);

    // ── Phase 1: READ ──────────────────────────────────────────
    uint64_t start_clock = *global_clock;
    int my_reads = 0;
    for (int r = 0; r < reads_per_thread; r++) {
        int idx = (lane * reads_per_thread + r) % num_addrs;
        uint32_t lw = lock_table[idx];
        if (gpu_epcc_is_locked(lw)) {
            uint32_t holder_prio = gpu_epcc_get_priority(lw);
            if (holder_prio > my_priority) *s_abort = 1;
        }
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
    if (*s_abort) goto abort_tx;

    // Wrap rest in scoped block to avoid goto-bypass over inits on HIP
    {
    // ── Phase 2: WRITE buffer ──────────────────────────────────
    int my_writes = 0;
    for (int w = 0; w < writes_per_thread; w++) {
        int idx = (lane * writes_per_thread + w + 1) % num_addrs;
        my_write_addr[my_writes] = (uint32_t)idx;
        my_writes++;
    }
    __syncwarp();

    // ── Phase 3: VALIDATE ──────────────────────────────────────
    int local_valid = 1;
    for (int i = 0; i < my_reads; i++) {
        uint32_t lw = lock_table[my_read_addr[i]];
        if (lw != my_read_ver[i]) { local_valid = 0; }
    }

    m = __ballot_sync(~0ULL, local_valid == 0);
    if (m) goto abort_tx;

    // ── Phase 4: LOCK (priority-based) ─────────────────────────
    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_addr[w];
        uint32_t old = lock_table[addr_idx];
        if (gpu_epcc_is_locked(old)) {
            uint32_t holder_prio = gpu_epcc_get_priority(old);
            if (holder_prio > my_priority) { *s_abort = 1; break; }
        }
    }

    local_abort = *s_abort;
    m = __ballot_sync(~0ULL, local_abort != 0);
    if (m) goto abort_tx;

    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_addr[w];
        uint32_t expected = 0;
        uint32_t desired = gpu_epcc_make_entry(my_priority, 0);
        uint32_t old = atomicCAS(&lock_table[addr_idx], expected, desired);
        if (old != 0) {
            uint32_t holder_prio = gpu_epcc_get_priority(old);
            if (holder_prio > my_priority) { *s_abort = 1; }
            else {
                for (int s = 0; s < 10; s++) {
                    expected = 0;
                    old = atomicCAS(&lock_table[addr_idx], expected, desired);
                    if (old == 0) break;
                    if (s == 9) *s_abort = 1;
                }
            }
        }
    }

    __syncwarp();
    local_abort = *s_abort;
    m = __ballot_sync(~0ULL, local_abort != 0);
    if (m) {
        for (int w = 0; w < my_writes; w++) {
            int addr_idx = (int)my_write_addr[w];
            uint32_t expected = gpu_epcc_make_entry(my_priority, 0);
            atomicCAS(&lock_table[addr_idx], expected, 0u);
        }
        goto abort_tx;
    }

    // ── Phase 5: COMMIT ────────────────────────────────────────
    __threadfence();
    if (lane == 0) atomicAdd((unsigned long long*)global_clock, 1ULL);
    __syncwarp();
    uint64_t commit_clock = *global_clock;

    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_addr[w];
        data[addr_idx] = (uint32_t)(lane + warp * 32 + w);
    }
    __threadfence();

    for (int w = 0; w < my_writes; w++) {
        int addr_idx = (int)my_write_addr[w];
        lock_table[addr_idx] = (uint32_t)commit_clock;
    }

    if (lane == 0) {
        atomicAdd((unsigned long long*)committed_count, 1ULL);
        if (warp < 1024) gpu_epcc_abort_counter[warp] = 0;
    }
    return;
    }

abort_tx:
    if (lane == 0) {
        atomicAdd((unsigned long long*)abort_count, 1ULL);
        if (warp < 1024) gpu_epcc_abort_counter[warp]++;
    }
}
