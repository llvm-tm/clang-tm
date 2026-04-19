#ifndef _ATOMIC_H_
#define _ATOMIC_H_

#include <stdint.h>

#if defined(USE_MODERN_ATOMIC) && (defined(__GNUC__) || defined(__clang__))

#include <cstdint>
#include <atomic>

typedef uintptr_t AO_t;
typedef AO_t atomic_t;

#define ATOMIC_CB                     std::atomic_thread_fence(std::memory_order_seq_cst)

#define ATOMIC_CAS_FULL(a, e, v)      ({ \
    AO_t _old = (e); \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_compare_exchange_strong_explicit(_a, &_old, (v), std::memory_order_seq_cst, std::memory_order_seq_cst) ? (e) : _old; \
})

#define ATOMIC_FETCH_INC_FULL(a)        ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_fetch_add_explicit(_a, 1, std::memory_order_seq_cst); \
})

#define ATOMIC_FETCH_DEC_FULL(a)      ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_fetch_sub_explicit(_a, 1, std::memory_order_seq_cst); \
})

#define ATOMIC_FETCH_ADD_FULL(a, v)   ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_fetch_add_explicit(_a, (v), std::memory_order_seq_cst); \
})

#define ATOMIC_LOAD_ACQ(a)            ({ \
    const std::atomic<AO_t>* _a = reinterpret_cast<const std::atomic<AO_t>*>(a); \
    std::atomic_load_explicit(_a, std::memory_order_acquire); \
})

#define ATOMIC_LOAD(a)                ({ \
    const std::atomic<AO_t>* _a = reinterpret_cast<const std::atomic<AO_t>*>(a); \
    std::atomic_load_explicit(_a, std::memory_order_relaxed); \
})

#define ATOMIC_STORE_REL(a, v)        ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_store_explicit(_a, (v), std::memory_order_release); \
})

#define ATOMIC_STORE(a, v)            ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_store_explicit(_a, (v), std::memory_order_relaxed); \
})

#define ATOMIC_MB_READ                std::atomic_thread_fence(std::memory_order_acquire)
#define ATOMIC_MB_WRITE               std::atomic_thread_fence(std::memory_order_release)
#define ATOMIC_MB_FULL               std::atomic_thread_fence(std::memory_order_seq_cst)

#elif defined(__GNUC__) || defined(__clang__)

#include "atomic_ops/atomic_ops.h"

typedef AO_t atomic_t;

#define ATOMIC_CB                     AO_compiler_barrier()
#define ATOMIC_CAS_FULL(a, e, v)      AO_compare_and_swap_full((volatile AO_t*)(a), (AO_t)(e), (AO_t)(v))
#define ATOMIC_FETCH_INC_FULL(a)      AO_fetch_and_add1_full((volatile AO_t*)(a))
#define ATOMIC_FETCH_DEC_FULL(a)      AO_fetch_and_sub1_full((volatile AO_t*)(a))
#define ATOMIC_FETCH_ADD_FULL(a, v)   AO_fetch_and_add_full((volatile AO_t*)(a), (AO_t)(v))

#ifdef SAFE
#define ATOMIC_LOAD_ACQ(a)           AO_load_full((volatile AO_t*)(a))
#define ATOMIC_LOAD(a)                AO_load_full((volatile AO_t*)(a))
#define ATOMIC_STORE_REL(a, v)        AO_store_full((volatile AO_t*)(a), (AO_t)(v))
#define ATOMIC_STORE(a, v)            AO_store_full((volatile AO_t*)(a), (AO_t)(v))
#else
#define ATOMIC_LOAD_ACQ(a)           AO_load_acquire_read((volatile AO_t*)(a))
#define ATOMIC_LOAD(a)               (*((volatile AO_t*)(a)))
#define ATOMIC_STORE_REL(a, v)        AO_store_release((volatile AO_t*)(a), (AO_t)(v))
#define ATOMIC_STORE(a, v)           (*((volatile AO_t*)(a)) = (AO_t)(v))
#endif

#define ATOMIC_MB_READ                AO_nop_read()
#define ATOMIC_MB_WRITE               AO_nop_write()
#define ATOMIC_MB_FULL                AO_nop_full()

#else

#include <cstdint>
#include <atomic>

typedef uintptr_t AO_t;
typedef AO_t atomic_t;

#define ATOMIC_CB                     std::atomic_thread_fence(std::memory_order_seq_cst)

#define ATOMIC_CAS_FULL(a, e, v)      ({ \
    AO_t _old = (e); \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_compare_exchange_strong_explicit(_a, &_old, (v), std::memory_order_seq_cst, std::memory_order_seq_cst) ? (e) : _old; \
})

#define ATOMIC_FETCH_INC_FULL(a)        ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_fetch_add_explicit(_a, 1, std::memory_order_seq_cst); \
})

#define ATOMIC_FETCH_DEC_FULL(a)      ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_fetch_sub_explicit(_a, 1, std::memory_order_seq_cst); \
})

#define ATOMIC_FETCH_ADD_FULL(a, v)   ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_fetch_add_explicit(_a, (v), std::memory_order_seq_cst); \
})

#define ATOMIC_LOAD_ACQ(a)            ({ \
    const std::atomic<AO_t>* _a = reinterpret_cast<const std::atomic<AO_t>*>(a); \
    std::atomic_load_explicit(_a, std::memory_order_acquire); \
})

#define ATOMIC_LOAD(a)                ({ \
    const std::atomic<AO_t>* _a = reinterpret_cast<const std::atomic<AO_t>*>(a); \
    std::atomic_load_explicit(_a, std::memory_order_relaxed); \
})

#define ATOMIC_STORE_REL(a, v)        ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_store_explicit(_a, (v), std::memory_order_release); \
})

#define ATOMIC_STORE(a, v)            ({ \
    std::atomic<AO_t>* _a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(a)); \
    std::atomic_store_explicit(_a, (v), std::memory_order_relaxed); \
})

#define ATOMIC_MB_READ                std::atomic_thread_fence(std::memory_order_acquire)
#define ATOMIC_MB_WRITE               std::atomic_thread_fence(std::memory_order_release)
#define ATOMIC_MB_FULL               std::atomic_thread_fence(std::memory_order_seq_cst)

#endif

#endif