/**
 * tm_region_allocator.cpp — TM Address-Space Region Allocator
 *
 * Manages a single mmap'd virtual region used for all TM-tracked
 * allocations.  Provides tm_region_init / tm_region_destroy and
 * the global bump-pointer variables referenced by the inline
 * functions in tm_region_allocator.hpp.
 *
 * The region is initialized once (lazily on first tm_malloc or
 * at tm_init/tm_queue_init time).  Thread-safe via atomic bump.
 */

#include "../tm_region_allocator.hpp"
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace stm {

// ── Global state ──────────────────────────────────────────────
char *g_tm_region_start = nullptr;
char *g_tm_region_end   = nullptr;
char *g_tm_region_bump  = nullptr;

// Guard to ensure single-init across all translation units.
static std::atomic<int> g_region_inited{0};

int tm_region_init() noexcept {
    if (g_region_inited.load(std::memory_order_acquire))
        return 0;

    // Try to init; only one thread succeeds.
    int expected = 0;
    if (!g_region_inited.compare_exchange_strong(expected, 1,
                                                  std::memory_order_acq_rel))
        return 0; // another thread beat us

    // Determine page size for aligment
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    size_t region_size = TM_REGION_SIZE;

    // On 32-bit, reduce region size to avoid virtual address space exhaustion
    if (sizeof(void *) == 4) {
        region_size = 512ULL * 1024 * 1024; // 512 MB
    }

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
    g_tm_region_bump  = g_tm_region_start;

    fprintf(stderr, "[TM-REGION] mmap %p .. %p  (%zu MB)\n",
            (void*)g_tm_region_start, (void*)g_tm_region_end,
            region_size / (1024 * 1024));

    return 0;
}

void tm_region_destroy() noexcept {
    if (!g_tm_region_start)
        return;

    munmap(g_tm_region_start, g_tm_region_end - g_tm_region_start);
    g_tm_region_start = nullptr;
    g_tm_region_end   = nullptr;
    g_tm_region_bump  = nullptr;
    g_region_inited.store(0, std::memory_order_release);
}

} // namespace stm
