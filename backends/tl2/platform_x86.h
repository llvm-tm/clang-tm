#ifndef PLATFORM_X86_H
#define PLATFORM_X86_H 1

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

#define MEMBARLDLD()                    /* nothing */
#define MEMBARSTST()                  /* nothing */
#define MEMBARSTLD()                  std::atomic_thread_fence(std::memory_order_seq_cst)

#ifndef ARCH_HAS_PREFETCHW
__INLINE__ void
prefetchw (volatile void* x)
{
    /* nothing */
}
#endif

#define LDNF(a)                         (*(a))

#define PAUSE()                         /* nothing */

#define TL2_TIMER_READ() ({ \
    std::chrono::high_resolution_clock::now().time_since_epoch().count(); \
})

#endif