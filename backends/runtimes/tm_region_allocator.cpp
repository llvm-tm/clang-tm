/**
 * tm_region_allocator.cpp — TM Address-Space Region Allocator
 *
 * Manages a single mmap'd virtual region for all TM-tracked
 * allocations.  The region is divided into fixed-size slabs
 * (g_slab_size = TM_SLAB_PAGES × page_size).  Each thread bumps
 * from its own thread-local slab; the slow path atomically claims
 * the next slab index.
 *
 * Single-init guarded by g_region_inited (atomic CAS).
 */

#include "../tm_region_allocator.hpp"
#include "../tm_platform.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

namespace stm {

// ── Global state ──────────────────────────────────────────────
char *g_tm_region_start = nullptr;
char *g_tm_region_end   = nullptr;

// Guard to ensure single-init across all translation units.
static std::atomic<int> g_region_inited{0};

int tm_region_init() noexcept {
    if (g_region_inited.load(std::memory_order_acquire))
        return 0;

    int expected = 0;
    if (!g_region_inited.compare_exchange_strong(expected, 1,
                                                  std::memory_order_acq_rel))
        return 0; // another thread beat us

    // ── Platform page size ───────────────────────────────────
    long page_size = stm::tm_page_size();

    size_t region_size = TM_REGION_SIZE;

    // On 32-bit, reduce region size to avoid virtual address exhaustion
    if (sizeof(void *) == 4) {
        region_size = 512ULL * 1024 * 1024; // 512 MB
    }

    // ── Compute slab size ────────────────────────────────────
    // Slab size = TM_SLAB_PAGES × page_size, always a page multiple.
    g_slab_size = static_cast<size_t>(TM_SLAB_PAGES) *
                  static_cast<size_t>(page_size);

    // ── mmap the region ──────────────────────────────────────
    void *addr = mmap(nullptr, region_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "FATAL: tm_region_init mmap(%zu) failed: %s\n",
                region_size, strerror(errno));
        g_region_inited.store(0, std::memory_order_release);
        return -1;
    }

    g_tm_region_start = static_cast<char *>(addr);
    g_tm_region_end   = g_tm_region_start + region_size;

    // ── Compute number of slabs ──────────────────────────────
    g_num_slabs = region_size / g_slab_size;
    g_next_slab_idx.store(0, std::memory_order_relaxed);

    fprintf(stderr, "[TM-REGION] mmap %p .. %p  (%zu MB, %zu slabs of %zu B)\n",
            (void*)g_tm_region_start, (void*)g_tm_region_end,
            region_size / (1024 * 1024), g_num_slabs, g_slab_size);

    return 0;
}

void tm_region_destroy() noexcept {
    // Region addresses remain valid for isTMAddress() checks even
    // after destroy — the OS reclaims virtual memory on process exit.
    // We DO NOT clear g_tm_region_start/end because global destructors
    // (treap clear_subtree, etc.) may still read from the region.
    // At process exit the virtual pages are still resident (no new
    // physical pages are allocated to anything else).
    //
    // An explicit munmap is technically correct but causes crashes in
    // global destructors that traverse TM-allocated data structures.
}

} // namespace stm
