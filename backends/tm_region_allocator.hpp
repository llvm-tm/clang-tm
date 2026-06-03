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
// Replaces the negative isStackAddress() check with a positive
// isTMAddress(addr) check: an address is TM-tracked iff it falls
// within the pre-allocated mmap region.  This is simpler, faster,
// and equally correct for all backends.
//
// The region is a single mmap reservation (virtual, not committed)
// of TM_REGION_SIZE bytes.  A thread-safe bump pointer allocates
// within the region.  No free-list yet — for long-running workloads
// that exhaust the region, add a slab-based free list.
//
// All allocations inside TXs go through this region.  Allocations
// outside TXs (runtime internal data structures like read_set,
// write_set) use the standard heap.
// ═══════════════════════════════════════════════════════════════════

#ifndef TM_REGION_SIZE
/// 64 GB virtual reservation — the OS only commits pages on first touch.
#define TM_REGION_SIZE (64ULL * 1024 * 1024 * 1024)
#endif

// ── Globals (defined in tm_region_allocator.cpp) ──────────────
extern "C" {
    extern char *g_tm_region_start;  // mmap'd region base
    extern char *g_tm_region_end;    // base + size
    extern char *g_tm_region_bump;   // atomic bump pointer (next free byte)
}

// ── isTMAddress() — positive region membership check ──────────
//
// Returns true iff addr falls within the TM region.
// This is the gateway for ALL TM read/write instrumentation:
// if an address is NOT in the TM region, the backend bypasses TM
// (raw load/store, no lock/version/read-set/write-set).
//
// Inline — called on every tm_read/tm_write (hot path).
inline bool isTMAddress(const void *addr) noexcept {
    const char *a = static_cast<const char *>(addr);
    return a >= g_tm_region_start && a < g_tm_region_end;
}

// ── Region lifecycle ──────────────────────────────────────────
//
// tm_region_init() — one-time mmap.  Returns 0 on success, -1 on failure.
// Called from tm_init() (each runtime) and tm_queue_init().
//
// tm_region_destroy() — munmap.  Called at shutdown.

int tm_region_init() noexcept;
void tm_region_destroy() noexcept;

// ── Region allocator ──────────────────────────────────────────
//
// Bump allocator: tm_region_malloc bumps the atomic bump pointer.
// Alignment: 16 bytes (sufficient for all standard types).
// Free is a no-op for the bump allocator.
//
// TX-abort safety: spec_alloc tracking (in tm_alloc_overrides.hpp)
// keeps TX-local allocation lists.  On abort, we just forget the
// entries (no need to rewind the bump — the region will eventually
// wrap / exhaust on long runs; add a free-list when that happens).

inline void *tm_region_malloc(size_t sz) noexcept {
    // Align to 16 bytes
    sz = (sz + 15) & ~15ULL;
    char *p = __atomic_fetch_add(&g_tm_region_bump, sz, __ATOMIC_RELAXED);
    if (p + sz > g_tm_region_end) {
        fprintf(stderr, "FATAL: TM region exhausted (start=%p end=%p bump=%p +%zu)\n",
                (void*)g_tm_region_start, (void*)g_tm_region_end, (void*)p, sz);
        fflush(stderr);
        std::abort();
    }
    return p;
}

inline void *tm_region_calloc(size_t nmemb, size_t sz) noexcept {
    void *p = tm_region_malloc(nmemb * sz);
    __builtin_memset(p, 0, nmemb * sz);
    return p;
}

inline void *tm_region_realloc(void *ptr, size_t new_sz) noexcept {
    if (!ptr) return tm_region_malloc(new_sz);
    if (new_sz == 0) return nullptr;
    // Bump allocator: can't shrink or grow in place.  Allocate new, copy.
    // We don't know the old size here — caller must track it.
    // For simplicity, allocate new and let the caller handle copy logic.
    // tm_realloc in practice is rare in TM workloads.
    void *np = tm_region_malloc(new_sz);
    // Note: caller should also memcpy the old content if needed.
    // We can't do it here since we don't know old size.
    return np;
}

inline void tm_region_free(void * /*ptr*/) noexcept {
    // Bump allocator: no-op.
    // Memory is implicitly reused on region exhaustion / restart.
    // For long-running workloads with many alloc/free cycles,
    // add a free-list or slab allocator.
}

} // namespace stm
