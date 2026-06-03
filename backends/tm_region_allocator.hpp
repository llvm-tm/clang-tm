#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace stm {

// ═══════════════════════════════════════════════════════════════════
// tm_region_allocator.hpp — TM Address-Space Region Allocator
//
// Architecture:
//   A single contiguous 64 GB mmap region provides the TM address
//   space.  The region is divided into fixed-size slabs whose size
//   is a configurable multiple of the platform page size (default
//   256 pages).  Each thread has an exclusive current slab from
//   which it bump-allocates.  When a thread's slab is exhausted, it
//   claims the next slab index via an atomic fetch_add (no CAS, no
//   spin, no lock).
//
// Fast path (per-thread bump):
//   tm_region_malloc bumps a thread_local pointer.  No atomic op,
//   no cache-line ping-pong.  This is the same per-thread heap
//   design as Hoard (Berger et al., ASPLOS 2000) and TCMalloc:
//   each thread allocates from its own slab without synchronisation.
//
// Slow path (slab claim):
//   When the current slab runs out, the thread claims the next slab
//   index atomically.  The slab address is trivially computed:
//   g_tm_region_start + idx * g_slab_size.  Since all slabs live
//   within the single contiguous region, isTMAddress() remains a
//   simple pair of comparisons — no per-slab bounds table.
//
// Slab sizing:
//   Slab size = TM_SLAB_PAGES × platform page_size.  This ensures
//   every slab is page-aligned, which matters for TM implementations
//   that use page-level metadata (protection-based conflict detection,
//   hardware page-locking, huge pages, etc.).  The default 256 pages
//   → 1 MB on 4 KB-page systems, 4 MB on 16 KB-page arm64 systems.
//
// Free is a no-op (bump allocator property).  TX-abort cleanup is
// handled by spec_alloc tracking in tm_alloc_overrides.hpp, not by
// rewinding the bump.
//
// References:
//   - Hoard: Berger et al., "Hoard: A Scalable Memory Allocator for
//     Multithreaded Applications", ASPLOS 2000.
//   - TCMalloc: Sanjay Ghemawat, "TCMalloc: Thread-Caching Malloc",
//     Google 2005.  Per-thread caches + global heap.
//   - Gidenstam et al., "Allocators for Transactional Memory",
//     PODC 2008.  TM-specific bump with TX-scoped reset.
//   - Bonwick, "The Slab Allocator: An Object-Caching Kernel Memory
//     Allocator", USENIX 1994.  Per-size-class object caches.
// ═══════════════════════════════════════════════════════════════════

#ifndef TM_REGION_SIZE
/// 64 GB virtual reservation — the OS only commits pages on first touch.
#define TM_REGION_SIZE (64ULL * 1024 * 1024 * 1024)
#endif

#ifndef TM_SLAB_PAGES
/// Number of pages per slab.  Slab size = pages × platform page_size.
/// Default 256 → 1 MB at 4 KB pages, 4 MB at 16 KB pages.
#define TM_SLAB_PAGES 256
#endif

// ── Globals (defined in tm_region_allocator.cpp) ──────────────
extern "C" {
    extern char *g_tm_region_start;  // mmap'd region base
    extern char *g_tm_region_end;    // base + size
}

// Slab size in bytes (computed at init from TM_SLAB_PAGES × page_size).
// Read by slow path only; fast path never references it.
inline size_t g_slab_size = 0;

// ── isTMAddress() — positive region membership check ──────────
// Fast path: two comparisons against the single contiguous region.
// Does NOT consult slab boundaries — all slabs fall within the region.
inline bool isTMAddress(const void *addr) noexcept {
    const char *a = static_cast<const char *>(addr);
    return a >= g_tm_region_start && a < g_tm_region_end;
}

// ── Region lifecycle ──────────────────────────────────────────
int tm_region_init() noexcept;
void tm_region_destroy() noexcept;

// ── Per-thread slab bump allocator ────────────────────────────
//
// Each thread has a thread-local current slab descriptor:
//   bump  — next free byte (bumped non-atomically on fast path)
//   end   — one past the last usable byte in this slab
//
// Slow path claims the next slab from a global counter and sets up
// the thread-local (bump, end) for subsequent fast-path allocs.
//
// Oversized allocations (sz > g_slab_size) claim ceil(sz/g_slab_size)
// consecutive slabs so the caller gets a contiguous block.

struct Slab {
    char *bump;
    char *end;
};

inline thread_local Slab g_tl_slab{nullptr, nullptr};

/// Global slab index counter.  Threads atomically claim slab indices
/// via fetch_add (no CAS, no spin, no lock).
inline std::atomic<size_t> g_next_slab_idx{0};

/// Total number of slabs in the region (set by tm_region_init).
inline size_t g_num_slabs = 0;

// Slow path — out-of-line to keep fast path compact.
inline void *tm_region_slow_alloc(size_t sz) noexcept;

inline void *tm_region_malloc(size_t sz) noexcept {
    // Align to 16 bytes (sufficient for all standard types).
    // Minimum 16 bytes so malloc(0) returns a unique pointer per C standard.
    if (sz < 16) sz = 16;
    sz = (sz + 15) & ~15ULL;

    // Fast path: non-atomic bump within the current slab.
    char *p = g_tl_slab.bump;
    char *next = p + sz;
    if (next <= g_tl_slab.end) [[likely]] {
        g_tl_slab.bump = next;
        return p;
    }

    // Slow path: current slab exhausted → claim a new one.
    return tm_region_slow_alloc(sz);
}

inline void *tm_region_slow_alloc(size_t sz) noexcept {
    // Oversized allocation: claim multiple consecutive slabs so
    // the returned block is contiguous within the region.
    size_t slab_sz = g_slab_size;
    size_t needed = 1;
    if (sz > slab_sz)
        needed = (sz + slab_sz - 1) / slab_sz;

    size_t idx = g_next_slab_idx.fetch_add(needed, std::memory_order_relaxed);
    char *slab = g_tm_region_start + idx * slab_sz;
    char *slab_end = slab + needed * slab_sz;

    if (slab_end > g_tm_region_end) [[unlikely]] {
        fprintf(stderr, "FATAL: TM region exhausted (start=%p end=%p "
                        "slab_idx=%zu needed=%zu slab_sz=%zu)\n",
                (void*)g_tm_region_start, (void*)g_tm_region_end,
                idx, needed, slab_sz);
        fflush(stderr);
        std::abort();
    }

    g_tl_slab.bump = slab + sz;
    g_tl_slab.end  = slab_end;
    return slab;
}

inline void *tm_region_calloc(size_t nmemb, size_t sz) noexcept {
    void *p = tm_region_malloc(nmemb * sz);
    __builtin_memset(p, 0, nmemb * sz);
    return p;
}

inline void *tm_region_realloc(void *ptr, size_t new_sz) noexcept {
    if (!ptr) return tm_region_malloc(new_sz);
    if (new_sz == 0) return nullptr;
    void *np = tm_region_malloc(new_sz);
    return np;
}

inline void tm_region_free(void * /*ptr*/) noexcept {
    // Bump allocator: no-op.  Slabs are never individually freed.
    // For long-running workloads that allocate+free at steady state,
    // add a thread-exit hook that returns the thread's slab index to
    // a lock-free stack; slow_alloc can pop from it first.
}

} // namespace stm
