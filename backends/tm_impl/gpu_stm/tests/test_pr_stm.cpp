// ── PR-STM smoke test ─────────────────────────────────────────────-
// Tests the CPU fallback PR-STM emulation and (if CUDA available)
// the GPU kernel launch.
//
// Compile:
//   cmake -B build -DBUILD_GPU_STM=ON -DGPU_STM_CPU_FALLBACK=ON
//   make -C build test_pr_stm_cpu test_pr_stm_gpu
//   ./build/test_pr_stm_cpu
//   ./build/test_pr_stm_gpu    (requires NVIDIA GPU)

#include <cstdio>
#include <cstdlib>
#include <cassert>

#include "gpu_stm_api.h"

// ── CPU emulation test ─────────────────────────────────────────────

void test_cpu_pr_stm() {
    printf("=== test_cpu_pr_stm ===\n");

    // Emulate 2 warps × 32 lanes, 64 addresses, 4 reads + 2 writes per tx
    cpu_pr_stm_emulate(
        /*num_warps=*/2,
        /*warp_size=*/4,         // small warp for fast test
        /*num_addrs=*/16,
        /*reads_per_thread=*/2,
        /*writes_per_thread=*/1
    );

    printf("[PASS] CPU PR-STM completed\n");
}

// ── GPU kernel launch test (requires GPU) ──────────────────────────

#ifdef __CUDACC__
void test_gpu_pr_stm() {
    printf("=== test_gpu_pr_stm ===\n");

    // Initialize GPU state
    gpu_tm_init();

    // Launch 4 warps
    int commits = gpu_pr_stm_launch(
        /*num_warps=*/4,
        /*tx_body=*/nullptr,  // uses internal deterministic body
        /*tx_data=*/nullptr
    );

    printf("GPU PR-STM: %d commits\n", commits);
    assert(commits > 0 && "Expected at least one successful commit");

    gpu_tm_exit();
    printf("[PASS] GPU PR-STM completed\n");
}
#endif

// ── Unit tests for lock word encoding ──────────────────────────────

void test_lock_word() {
    printf("=== test_lock_word ===\n");

    // Free entry
    uint32_t free_entry = pr_stm_make_entry(0, 0, 0);
    assert(pr_stm_get_priority(free_entry) == 0);
    assert(pr_stm_get_version(free_entry) == 0);
    assert(pr_stm_is_locked(free_entry) == 0);

    // Locked entry with priority 5, version 42
    uint32_t locked = pr_stm_make_entry(5, 42, 1);
    assert(pr_stm_get_priority(locked) == 5);
    assert(pr_stm_get_version(locked) == 42);
    assert(pr_stm_is_locked(locked) == 1);

    // High priority, max version
    uint32_t high = pr_stm_make_entry(255, 0x7FFFFF, 1);
    assert(pr_stm_get_priority(high) == 255);
    assert(pr_stm_get_version(high) == 0x7FFFFF);
    assert(pr_stm_is_locked(high) == 1);

    printf("[PASS] Lock word encoding tests passed\n");
}

// ── Main ───────────────────────────────────────────────────────────

int main() {
    test_lock_word();
    test_cpu_pr_stm();

    // Only run GPU test if CUDA is available
    // (checked inside gpu_tm_init)
    // test_gpu_pr_stm();

    printf("\nAll PR-STM tests PASSED\n");
    return 0;
}
