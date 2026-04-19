#ifndef PLATFORM_SPARC_H
#define PLATFORM_SPARC_H 1

#ifndef PLATFORM_H
#  error include "platform.h" for "platform_sparc.h"
#endif

#include <atomic>
#include <cstdint>
#include "common.h"

__INLINE__ intptr_t
cas (intptr_t newVal, intptr_t oldVal, volatile intptr_t* ptr)
{
    std::atomic<intptr_t> *a = reinterpret_cast<std::atomic<intptr_t>*>(ptr);
    std::atomic<intptr_t> expected(oldVal);
    a->compare_exchange_strong(expected, newVal, std::memory_order_seq_cst);
    return expected.load(std::memory_order_seq_cst);
}

#define MEMBARLDLD()   /* nothing */
#define MEMBARSTST()   /* nothing */
#define MEMBARSTLD()   std::atomic_thread_fence(std::memory_order_seq_cst)

__INLINE__ void
prefetchw (volatile void* x)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(x, 1, 3);
#endif
}

__INLINE__ intptr_t
LDNF (volatile intptr_t* a)
{
    return *a;
}

#define PAUSE()  /* nothing */

#define TL2_TIMER_READ() gethrtime()

#endif