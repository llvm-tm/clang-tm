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
 * NOTE: Persistent pointers:
 * Even with allocations redirected to the persistent mmap, std::map's
 * internal tree nodes store RAW C++ POINTERS to other nodes.  After a
 * restart those pointers dangle (ASLR changes the mmap base address).
 * Use either (a) fixed-address mmap, (b) RelPtr-based containers, or
 * (c) serialisation to arrays (see benchmarks/persistent_kv_stdmap.cpp).
 */

#include <cstddef>
#include <new>

// Each runtime must define these two functions:
extern "C" void* tm_malloc(size_t size);
extern "C" void  tm_free(void* ptr);

// Thread-local flag: set by tm_begin/tm_end to distinguish TX vs non-TX context.
// tm_malloc checks this to decide whether to allocate from the TM runtime
// (in_transaction) or from the system heap (non-transactional).
extern thread_local bool g_in_tx;

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
