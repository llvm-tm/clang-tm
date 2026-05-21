#pragma once

/**
 * tm_alloc_overrides.hpp — C++ operator new/delete overrides
 *
 * Each runtime .cpp file should #include this header to redirect ALL
 * C++ heap allocations (new, delete, new[], delete[], and their sized,
 * aligned, nothrow variants) through the runtime's tm_malloc/tm_free.
 *
 * This is necessary because std::map, std::vector, and other STL
 * containers allocate internally via operator new/delete.  Without this
 * override, those allocations go to the process heap and are invisible
 * to persistent/shared-memory backends.
 *
 * The C malloc/free/calloc/realloc are handled separately by the LLVM
 * plugin pass which replaces them with tm_malloc/tm_free calls.
 *
 * DEFERRED FREE (transaction-safety):
 * Inside a transaction, operator delete → tm_free pushes the pointer
 * onto a thread-local deferred-free list rather than calling ::free
 * immediately.  This prevents the "vector reallocation double-free"
 * scenario: if a transaction aborts, the undo log restores container
 * pointers to pre-reallocation values, and the old (deferred-free'd)
 * buffer is still alive because the free was never executed.
 *
 * Each runtime must call:
 *   tm_clear_deferred_frees()  at the start of tm_begin (outer)
 *   tm_flush_deferred_frees()  after successful commit in tm_end (outer)
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
#include <new>

// Each runtime must define these two functions:
extern "C" void* tm_malloc(size_t size);
extern "C" void  tm_free(void* ptr);

// Thread-local flag: set by tm_begin/tm_end to distinguish TX vs non-TX context.
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
    SpecAlloc* next;
    void* ptr;             // the speculatively-allocated memory
};

extern thread_local SpecAlloc* g_spec_allocs;

// Track a malloc/calloc/realloc result as a speculative allocation.
inline void tm_track_spec_alloc(void* ptr)
{
    if (g_in_tx && ptr) {
        auto* node = static_cast<SpecAlloc*>(std::malloc(sizeof(SpecAlloc)));
        node->ptr = ptr;
        node->next = g_spec_allocs;
        g_spec_allocs = node;
    }
}

// Free all speculative allocations and their bookkeeping — call on abort
// (from tm_begin, after a previous TX attempt failed).
inline void tm_clear_spec_allocs()
{
    auto* node = g_spec_allocs;
    while (node) {
        auto* next = node->next;
        std::free(node->ptr);   // free the speculatively-allocated memory
        std::free(node);        // free the bookkeeping node
        node = next;
    }
    g_spec_allocs = nullptr;
}

// Free only the bookkeeping — call on commit.  The allocated memory is now
// owned by the data structure and must NOT be freed here.
inline void tm_flush_spec_allocs()
{
    auto* node = g_spec_allocs;
    while (node) {
        auto* next = node->next;
        std::free(node);        // free only the bookkeeping node
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
    FreeNode* next;
    void* ptr;           // the user pointer to be freed on commit
};

extern thread_local FreeNode* g_deferred_frees;

// Flush (execute) all pending deferred frees — call after successful commit.
// Frees both the user pointer AND the bookkeeping FreeNode.
inline void tm_flush_deferred_frees()
{
    auto* node = g_deferred_frees;
    while (node) {
        auto* next = node->next;
        std::free(node->ptr);
        std::free(node);
        node = next;
    }
    g_deferred_frees = nullptr;
}

// Discard all pending deferred frees — call at tm_begin (outer) to discard
// entries from a previously aborted transaction attempt.  The undo log has
// restored the container pointers, so the deferred buffer should not be
// freed (it is live again).  Only the bookkeeping FreeNode is reclaimed.
inline void tm_clear_deferred_frees()
{
    auto* node = g_deferred_frees;
    while (node) {
        auto* next = node->next;
        std::free(node);   // free the FreeNode, NOT the user pointer
        node = next;
    }
    g_deferred_frees = nullptr;
}

// ── operator new ───────────────────────────────────────────

void* operator new(size_t size)                     { return tm_malloc(size); }
void* operator new(size_t size, std::nothrow_t const&) noexcept { return tm_malloc(size); }
void* operator new[](size_t size)                   { return tm_malloc(size); }
void* operator new[](size_t size, std::nothrow_t const&) noexcept { return tm_malloc(size); }

// C++17 aligned new
void* operator new(size_t size, std::align_val_t align) {
    (void)align;  // our tm_malloc returns sufficiently aligned memory
    return tm_malloc(size);
}
void* operator new(size_t size, std::align_val_t align, std::nothrow_t const&) noexcept {
    (void)align; return tm_malloc(size);
}
void* operator new[](size_t size, std::align_val_t align) {
    (void)align; return tm_malloc(size);
}
void* operator new[](size_t size, std::align_val_t align, std::nothrow_t const&) noexcept {
    (void)align; return tm_malloc(size);
}

// ── operator delete ────────────────────────────────────────

void operator delete(void* ptr) noexcept                        { tm_free(ptr); }
void operator delete(void* ptr, std::nothrow_t const&) noexcept { tm_free(ptr); }
void operator delete[](void* ptr) noexcept                      { tm_free(ptr); }
void operator delete[](void* ptr, std::nothrow_t const&) noexcept { tm_free(ptr); }

// Sized delete (C++14)
void operator delete(void* ptr, size_t) noexcept   { tm_free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { tm_free(ptr); }

// Aligned delete (C++17)
void operator delete(void* ptr, std::align_val_t) noexcept                  { tm_free(ptr); }
void operator delete(void* ptr, std::align_val_t, std::nothrow_t const&) noexcept { tm_free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept                { tm_free(ptr); }
void operator delete[](void* ptr, std::align_val_t, std::nothrow_t const&) noexcept { tm_free(ptr); }
void operator delete(void* ptr, size_t, std::align_val_t) noexcept          { tm_free(ptr); }
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept        { tm_free(ptr); }
