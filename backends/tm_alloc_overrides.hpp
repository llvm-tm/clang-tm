#pragma once

/**
 * tm_alloc_overrides.hpp — Deferred-free / speculative allocation helpers
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  CRITICAL: The runtime's own data structures MUST NOT be tracked					║
 * ║  as speculative allocations.  The read_set, write_set, and their					║
 * ║  internal bucket arrays (std::unordered_map) use the STANDARD						║
 * ║  ::operator new/delete directly.  No operator new/delete overrides					║
 * ║  are provided here - doing so caused a use-after-free on abort:					║
 * ║  the bucket arrays were spec-tracked via tm_malloc, then freed						║
 * ║  by tm_clear_spec_allocs() while the unordered_map still held						║
 * ║  pointers to them.  See commit <FIX>.												║
 * ║																					║
 * ║  CORRECT: The LLVM TM plugin replaces operator new/delete in							║
 * ║  instrumented user code at the IR level, not via header overrides.				║
 * ║  Runtime code (compiled separately, no plugin) uses standard allocator.			║
 * ║																					║
 * ║  operator delete overrides ARE provided to handle bump-region addresses				║
 * ║  from tm_malloc that survive TX commit and are later deleted outside TX.			║
 * ║  These overrides are safe for runtime internals because they use heap.			║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * This header provides:
 *   — tm_malloc / tm_free / tm_calloc / tm_realloc   (user alloc wrappers)
 *   — tm_track_spec_alloc / tm_clear_spec_allocs / tm_flush_spec_allocs
 *   — tm_flush_deferred_frees / tm_clear_deferred_frees
 *   — operator new/delete overrides   (INTENTIONALLY OMITTED — see above)
 *
 * The runtime code (compiled in a separate clang++ invocation, NEVER fed
 * through the plugin) uses the standard allocator for its internal data.
 * The plugin-instrumented user code uses explicit tm_malloc/tm_free calls
 * inserted by the pass, not hidden operator new/delete overrides.
 *
 * DEFERRED FREE (transaction-safety):
 * Inside a transaction, tm_free pushes the pointer onto a thread-local
 * deferred-free list rather than calling ::free immediately.
 *
 * SPECULATIVE ALLOCATION (abort rollback):
 * Inside a transaction, tm_malloc adds the pointer to a thread-local
 * spec-alloc list.  On abort, tm_clear_spec_allocs frees both the
 * bookkeeping and the user data.  On commit, tm_flush_spec_allocs frees
 * only the bookkeeping — the user data is now permanent.
 *
 * Each runtime must call:
 *   tm_clear_spec_allocs()    at the start of tm_begin (outer)
 *   tm_clear_deferred_frees() at the start of tm_begin (outer)
 *   tm_flush_spec_allocs()    after successful commit in tm_end (outer)
 *   tm_flush_deferred_frees() after successful commit in tm_end (outer)
 *
 * NOTE: Persistent pointers:
 * Even with allocations redirected to the persistent mmap, std::map's
 * internal tree nodes store RAW C++ POINTERS to other nodes.  After a
 * restart those pointers dangle (ASLR changes the mmap base address).
 * Use either (a) fixed-address mmap, (b) RelPtr-based containers, or
 * (c) serialisation to arrays (see benchmarks/persistent_kv_stdmap.cpp).
 */

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdlib>
#include <new>
#include <unordered_set>

#include "tm_event_logger.hpp"
#include "tm_region_allocator.hpp"

// ── Thread-local state ───────────────────────────────────
// Each runtime file MUST define these (with `thread_local` not `extern`):
//
//   thread_local bool g_in_tx = false;
//   thread_local FreeNode* g_deferred_frees = nullptr;
//   thread_local std::unordered_set<void*> g_deferred_frees_set;
//   thread_local SpecAlloc* g_spec_allocs = nullptr;

// Each runtime must define these two functions:
extern "C" void *tm_malloc(size_t size);
extern "C" void tm_free(void *ptr);

extern thread_local bool g_in_tx;

// ── Speculative-allocation tracking ───────────────────────
//
// When tm_malloc is called inside a transaction, the allocated memory
// is "speculative": it will become permanent only if the transaction
// commits.  If the transaction aborts, the undo log restores container
// pointers to their pre-transaction values, and the speculatively-
// allocated memory must be freed (otherwise it leaks).
//
// This is the symmetric counterpart of the deferred-free list:
//
//     Deferred-free (tm_free):  commit → execute free;  abort → leak (don't free)
//     Speculative (tm_malloc):  commit → keep (don't free);  abort → execute free
//
// Each runtime must call:
//   tm_clear_spec_allocs()   in tm_begin (outer), before tm_clear_deferred_frees
//   tm_flush_spec_allocs()   in tm_end (outer), after commit, before tm_flush_deferred_frees

struct SpecAlloc {
	SpecAlloc *next;
	void *ptr; // the speculatively-allocated memory
};

extern thread_local SpecAlloc *g_spec_allocs;

// Track a malloc/calloc/realloc result as a speculative allocation.
inline void tm_track_spec_alloc(void *ptr)
{
	if (g_in_tx && ptr) {
		auto *node = static_cast<SpecAlloc *>(std::malloc(sizeof(SpecAlloc)));
		node->ptr = ptr;
		node->next = g_spec_allocs;
		g_spec_allocs = node;
	}
}

// Mark a speculatively-allocated pointer as explicitly freed (via tm_free).
// Sets node->ptr to nullptr so tm_clear_spec_allocs does NOT double-free it.
inline void tm_untrack_spec_alloc(void *ptr)
{
	if (!g_in_tx || !ptr)
		return;
	auto *node = g_spec_allocs;
	while (node) {
		if (node->ptr == ptr) {
			node->ptr = nullptr; // prevent double-free on abort
			return;
		}
		node = node->next;
	}
}

// Free all speculative allocations and their bookkeeping — call on abort
// (from tm_begin, after a previous TX attempt failed).
// Entries whose ptr is nullptr have been explicitly freed via tm_free
// and are skipped — their deallocation will be handled on commit.
inline void tm_clear_spec_allocs()
{
	auto *node = g_spec_allocs;
	while (node) {
		auto *next = node->next;
		if (node->ptr) {
			TM_EVENT(CLEAR_SPEC_ALLOC, (uintptr_t)node->ptr, 0);
			stm::tm_region_free(node->ptr);
		}
		std::free(node);                  // free the bookkeeping node (std::malloc'd)
		node = next;
	}
	g_spec_allocs = nullptr;
}

// Free only the bookkeeping — call on commit.  The allocated memory is now
// owned by the data structure and must NOT be freed here.
inline void tm_flush_spec_allocs()
{
	auto *node = g_spec_allocs;
	while (node) {
		auto *next = node->next;
		std::free(node); // free only the bookkeeping node
		node = next;
	}
	g_spec_allocs = nullptr;
}

// ── Deferred-free infrastructure ──────────────────────────
//
// Uses a NON-INTRUSIVE singly-linked list — each FreeNode is a
// separately-allocated bookkeeping struct (16 bytes on 64-bit).
// This avoids the "intrusive-list corruption" problem: an intrusive
// list writes the next pointer into the freed block itself, which
// corrupts the first 8 bytes of the block.  If the transaction later
// aborts and the undo log resurrects the block (restores the container
// pointer), the corrupted data leads to a memory consistency violation
// (Lemma 8.3 in docs/proofs.md).
//
// ::malloc inside the deferred-free path is safe:
//   - This code runs in the runtime, which is NOT instrumented by the
//     TM plugin, so ::malloc is the real C library malloc.
//   - The alternative (std::vector::push_back) would call operator
//     delete → tm_free on reallocation, causing infinite recursion.

struct FreeNode {
	FreeNode *next;
	void *ptr; // the user pointer to be freed on commit
};

extern thread_local FreeNode *g_deferred_frees;

// Duplicate-detection set for deferred frees.
extern thread_local std::unordered_set<void *> g_deferred_frees_set;

// Flush (execute) all pending deferred frees — call after successful commit.
// Frees both the user pointer AND the bookkeeping FreeNode.
inline void tm_flush_deferred_frees()
{
	auto *node = g_deferred_frees;
	while (node) {
		auto *next = node->next;
		TM_EVENT(FLUSH_DEFERRED, (uintptr_t)node->ptr, 0);
		stm::tm_region_free(node->ptr);
		std::free(node);              // FreeNode bookkeeping (::malloc'd)
		node = next;
	}
	g_deferred_frees = nullptr;
	g_deferred_frees_set.clear();
}

// Discard all pending deferred frees — call at tm_begin (outer) to discard
// entries from a previously aborted transaction attempt.  The undo log has
// restored the container pointers, so the deferred buffer should not be
// freed (it is live again).  Only the bookkeeping FreeNode is reclaimed.
inline void tm_clear_deferred_frees()
{
	auto *node = g_deferred_frees;
	while (node) {
		auto *next = node->next;
		std::free(node); // free the FreeNode (::malloc'd), NOT the user pointer
		node = next;
	}
	g_deferred_frees = nullptr;
	g_deferred_frees_set.clear();
}

// ── Shared alloc/free bookkeeping ────────────────────────
//
// These functions encapsulate the deferred-free and spec-alloc bookkeeping
// that is identical across all TM backends (TinySTM, NOrec, SwissTM, TL2,
// DUDETM, NVHTM, SPHT).  Each runtime calls them from its per-backend thin
// wrapper, eliminating 7 copies of the same 30-line tm_free body.
//
// Usage in each runtime:
//
//   extern "C" void* tm_malloc(size_t s) {
//       return tm_track_alloc_result(::operator new(s), s);
//   }
//   extern "C" void  tm_free(void* ptr) {
//       if (!ptr) return;
//       TM_EVENT(FREE, ptr, 0);
//       if (g_in_tx) {
//           BACKEND::tm_write_i1((uint8_t*)ptr, 0);   // backend-specific
//           tm_free_append_deferred(ptr);
//       } else {
//           ::operator delete(ptr);                    // backend-specific
//       }
//   }

// Inside a TX, tm_malloc / tm_calloc / tm_realloc call this to record
// the allocation as speculative (freed on abort, kept on commit).
inline void* tm_track_alloc_result(void* p, size_t size) {
    TM_EVENT(MALLOC, p, size);
    tm_track_spec_alloc(p);
    return p;
}

// Inside a TX, tm_free calls this AFTER the backend-specific dummy write
// to append the address to the deferred-free list.  Shared logic:
//   - double-free detection (g_deferred_frees_set)
//   - untrack spec_alloc
//   - insert into g_deferred_frees_set
//   - push FreeNode onto g_deferred_frees
inline void tm_free_append_deferred(void* ptr) {
    if (g_deferred_frees_set.count(ptr)) {
        TM_EVENT(DOUBLE_FREE, ptr, 0);
        fprintf(stderr, "FATAL: double-free detected in TM: ptr=%p\n", ptr);
        fflush(stderr);
        stm::tm_backtrace_print(2);
        _exit(1);
    }
    tm_untrack_spec_alloc(ptr);
    g_deferred_frees_set.insert(ptr);
    auto* node = static_cast<FreeNode*>(std::malloc(sizeof(FreeNode)));
    node->ptr = ptr;
    node->next = g_deferred_frees;
    g_deferred_frees = node;
}

// ═══════════════════════════════════════════════════════════════════
//  operator new / delete overrides are intentionally NOT provided.
// ═══════════════════════════════════════════════════════════════════
//
// WHY: The runtime's own data structures (read_set, write_set,
//      unordered_map bucket arrays) MUST NOT go through tm_malloc.
//      If they do, every bucket-array allocation is spec-tracked, and
//      on abort, tm_clear_spec_allocs() frees the bucket array while
//      the unordered_map still holds a pointer to it → use-after-free
//      on the next transaction's clear() → heap corruption → SIGABRT.
//
//      This is NOT just theoretical — it was the root cause of the
//      bank benchmark crash at 2+ threads (all TinySTM variants).
//
// HOW user code allocs are handled: The LLVM TM plugin replaces
//   malloc/free/operator new/operator delete in instrumented user
//   code with explicit tm_malloc/tm_free calls.  This happens at the
//   IR level via the plugin pass, NOT via header overrides.  The
//   runtime code is compiled separately (no plugin), so its own
//   new/delete go to the standard allocator.  This is correct and
//   intentional.
//
// TL;DR: Runtime internals → standard allocator.
//        User TM code      → tm_malloc/tm_free (via plugin).


