/**
 * tm_region_allocator.cpp — TM Address-Space Region Allocator
 *
 * Manages a single mmap'd virtual region for all TM-tracked
 * allocations.  Provides per-thread free-list caching, page-based
 * chunks with bitmaps for small allocations, and freelists for
 * medium allocations.  Large allocations (> 4 KB) use slab bump
 * with a small header for free-path recycling.
 */

#include "tm_region_allocator.hpp"
#include "tm_platform.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/mman.h>

namespace stm {

// ── Global state ──────────────────────────────────────────
char *g_tm_region_start = nullptr;
char *g_tm_region_end   = nullptr;

size_t g_slab_size         = 0;
int    g_slab_size_shift   = 0;
size_t g_chunks_per_slab   = 0;

uint16_t g_sc_block_size[MAX_CLASSES];
uint16_t g_sc_block_count[MAX_CLASSES];
uint16_t g_sc_bitmap_bytes[MAX_CLASSES];
uint16_t g_sc_data_off[MAX_CLASSES];

std::atomic<size_t> g_next_slab_idx{0};
size_t g_num_slabs = 0;

thread_local Slab       g_tl_slab{nullptr, nullptr};
thread_local TLFreeList g_tl_free_lists[MAX_CLASSES]{};
thread_local void*      g_tl_hot_chunks[MAX_CLASSES]{};
thread_local LargeHdr*  g_tl_large_free_list{nullptr};

std::vector<TMGlobalRange> g_tm_globals;

thread_local const char *g_tm_stack_low = nullptr;
thread_local const char *g_tm_stack_high = nullptr;

static std::atomic<int> g_region_inited{0};

// ── Size class table (compile-time) ───────────────────────
static constexpr uint16_t kSizeTable[MAX_CLASSES] = {
    16,   24,   32,   40,   48,   56,   64,   80,
    96,  112,  128,  160,  192,  224,  256,  320,
   384,  448,  512,  640,  768,  896, 1024, 1280,
  1536, 1792, 2048, 2560, 3072, 3584, 4096, 0
};

// ── tm_region_init ────────────────────────────────────────

int tm_region_init() noexcept {
    if (g_region_inited.load(std::memory_order_acquire))
        return 0;

    int expected = 0;
    if (!g_region_inited.compare_exchange_strong(expected, 1,
                                                  std::memory_order_acq_rel))
        return 0;

    long page_size = stm::tm_page_size();

    size_t region_size = TM_REGION_SIZE;
    if (sizeof(void *) == 4)
        region_size = 512ULL * 1024 * 1024;

    // ── Compute slab size ─────────────────────────────────
    // Slab size = TM_SLAB_PAGES × page_size, always a page multiple.
    g_slab_size = static_cast<size_t>(TM_SLAB_PAGES) *
                  static_cast<size_t>(page_size);
    g_slab_size_shift = __builtin_ctzll(g_slab_size);
    static_assert((CHUNK_SIZE & (CHUNK_SIZE - 1)) == 0,
                  "CHUNK_SIZE must be power of 2");
    int chunk_shift = __builtin_ctzll((size_t)CHUNK_SIZE);
    g_chunks_per_slab = g_slab_size >> chunk_shift;

    // ── Precompute size class tables ──────────────────────
    for (int sc = 0; sc < (int)MAX_CLASSES; sc++) {
        uint16_t bs = kSizeTable[sc];
        if (bs == 0) {
            // Sentinel — no more classes
            g_sc_block_size[sc] = 0;
            g_sc_block_count[sc] = 0;
            g_sc_bitmap_bytes[sc] = 0;
            g_sc_data_off[sc] = 0;
            break;
        }

        g_sc_block_size[sc] = bs;

        if (bs <= BITMAP_THRESHOLD) {
            // Bitmap mode: compute max blocks that fit with bitmap overhead
            uint32_t max_try = (CHUNK_SIZE - CHUNK_HEADER_SZ) / bs;
            uint32_t bc;
            for (bc = max_try; bc > 0; bc--) {
                uint32_t bm_bytes = (bc + 7) >> 3;
                uint32_t hdr_total = CHUNK_HEADER_SZ + bm_bytes;
                // Round data offset up to page boundary (4096) so allocator
                // metadata (ChunkHeader, bitmap) never shares a 4 KB page
                // with data blocks.  This prevents backends that do full-page
                // write-back (e.g. XTM) from corrupting allocator metadata.
                uint32_t data_off = (hdr_total + 4095) & ~4095u;
                uint32_t total = data_off + bc * bs;
                if (total <= CHUNK_SIZE) {
                    g_sc_bitmap_bytes[sc] = (uint16_t)bm_bytes;
                    g_sc_data_off[sc]     = (uint16_t)data_off;
                    break;
                }
            }
            g_sc_block_count[sc] = (uint16_t)bc;
        } else {
            // Freelist mode: no bitmap overhead.
            // Start data at page boundary after header.
            uint32_t bc = (uint32_t)(CHUNK_SIZE - 4096u) / bs;
            g_sc_block_count[sc]  = (uint16_t)bc;
            g_sc_bitmap_bytes[sc] = 0;
            g_sc_data_off[sc]     = 4096u;
        }
    }

    // ── mmap the region ───────────────────────────────────
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

    // Align region start up to CHUNK_SIZE so chunk_start() produces
    // correct addresses.  Slab size (1 MB) is a multiple of CHUNK_SIZE
    // (64 KB), so slab boundaries are also CHUNK_SIZE-aligned.
    char *aligned_start = (char*)(((uintptr_t)addr + CHUNK_MASK) & ~CHUNK_MASK);
    size_t aligned_size = region_size & ~CHUNK_MASK; // round down
    if (aligned_start + aligned_size > (char*)addr + region_size)
        aligned_size -= CHUNK_SIZE; // paranoia: stay within mmap

    g_tm_region_start = aligned_start;
    g_tm_region_end   = aligned_start + aligned_size;

    g_num_slabs = aligned_size >> g_slab_size_shift;
    g_next_slab_idx.store(0, std::memory_order_relaxed);

    return 0;
}

// ── tm_region_destroy ─────────────────────────────────────

void tm_region_destroy() noexcept {
    // The OS reclaims the virtual mapping at process exit.
    // We deliberately do NOT reset g_next_slab_idx or any thread-local
    // state, because hot-chunk pointers from earlier allocations may
    // still reference valid headers.  Resetting the slab index would
    // cause later allocations to reuse old slabs, corrupting headers.
    // Tests that need full reset should call tm_region_init again
    // (which is idempotent and reuses the existing mapping).
}

// ── tm_register_global ──────────────────────────────────
// Called from instrumented main() for each TM-annotated global
// so that isTMGlobal() can check membership.

extern "C" void tm_register_global(void *addr, size_t size) {
    g_tm_globals.push_back({static_cast<const char *>(addr),
                            static_cast<const char *>(addr) + size});
}

} // namespace stm
