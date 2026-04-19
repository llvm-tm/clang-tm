#ifndef TM_ATOMIC_H
#define TM_ATOMIC_H

#include <atomic>
#include <cstdint>

namespace tm_atomic {

using atomic_t = std::atomic<uintptr_t>;

inline void compiler_barrier() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline bool compare_and_swap(atomic_t* addr, uintptr_t old_val, uintptr_t new_val) {
    uintptr_t expected = old_val;
    return addr->compare_exchange_strong(expected, new_val);
}

inline uintptr_t compare_and_swap_val(atomic_t* addr, uintptr_t old_val, uintptr_t new_val) {
    uintptr_t expected = old_val;
    addr->compare_exchange_strong(expected, new_val);
    return expected;
}

inline bool compare_and_swap_full(atomic_t* addr, uintptr_t old_val, uintptr_t new_val) {
    uintptr_t expected = old_val;
    return addr->compare_exchange_strong(expected, new_val, std::memory_order_seq_cst);
}

inline uintptr_t compare_and_swap_val_full(atomic_t* addr, uintptr_t old_val, uintptr_t new_val) {
    uintptr_t expected = old_val;
    addr->compare_exchange_strong(expected, new_val, std::memory_order_seq_cst);
    return expected;
}

inline bool compare_and_swap_acquire(atomic_t* addr, uintptr_t old_val, uintptr_t new_val) {
    uintptr_t expected = old_val;
    return addr->compare_exchange_strong(expected, new_val, std::memory_order_acquire);
}

inline bool compare_and_swap_release(atomic_t* addr, uintptr_t old_val, uintptr_t new_val) {
    uintptr_t expected = old_val;
    return addr->compare_exchange_strong(expected, new_val, std::memory_order_release);
}

inline uintptr_t fetch_and_add1(atomic_t* addr) {
    return addr->fetch_add(1, std::memory_order_relaxed);
}

inline uintptr_t fetch_and_add1_full(atomic_t* addr) {
    return addr->fetch_add(1, std::memory_order_seq_cst);
}

inline uintptr_t fetch_and_add1_acquire(atomic_t* addr) {
    return addr->fetch_add(1, std::memory_order_acquire);
}

inline uintptr_t fetch_and_add1_release(atomic_t* addr) {
    return addr->fetch_add(1, std::memory_order_release);
}

inline uintptr_t fetch_and_sub1(atomic_t* addr) {
    return addr->fetch_sub(1, std::memory_order_relaxed);
}

inline uintptr_t fetch_and_sub1_full(atomic_t* addr) {
    return addr->fetch_sub(1, std::memory_order_seq_cst);
}

inline uintptr_t fetch_and_add(atomic_t* addr, uintptr_t val) {
    return addr->fetch_add(val, std::memory_order_relaxed);
}

inline uintptr_t fetch_and_add_full(atomic_t* addr, uintptr_t val) {
    return addr->fetch_add(val, std::memory_order_seq_cst);
}

inline void store(atomic_t* addr, uintptr_t val) {
    addr->store(val, std::memory_order_relaxed);
}

inline void store_full(atomic_t* addr, uintptr_t val) {
    addr->store(val, std::memory_order_seq_cst);
}

inline void store_release(atomic_t* addr, uintptr_t val) {
    addr->store(val, std::memory_order_release);
}

inline uintptr_t load(atomic_t* addr) {
    return addr->load(std::memory_order_relaxed);
}

inline uintptr_t load_full(atomic_t* addr) {
    return addr->load(std::memory_order_seq_cst);
}

inline uintptr_t load_acquire(atomic_t* addr) {
    return addr->load(std::memory_order_acquire);
}

inline void nop_full() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void nop_read() {
    std::atomic_thread_fence(std::memory_order_acquire);
}

inline void nop_write() {
    std::atomic_thread_fence(std::memory_order_release);
}

}

#if defined(TM_USE_LEGACY_MACROS) || defined(ATOMIC_BUILTIN)
typedef uintptr_t atomic_t;
#endif

#if defined(TM_USE_LEGACY_MACROS)
#define ATOMIC_CB() std::atomic_thread_fence(std::memory_order_seq_cst)
#define ATOMIC_CAS_FULL(a, e, v) tm_atomic::compare_and_swap_val_full((tm_atomic::atomic_t*)(a), (uintptr_t)(e), (uintptr_t)(v))
#define ATOMIC_FETCH_INC_FULL(a) tm_atomic::fetch_and_add1_full((tm_atomic::atomic_t*)(a))
#define ATOMIC_FETCH_DEC_FULL(a) tm_atomic::fetch_and_sub1_full((tm_atomic::atomic_t*)(a))
#define ATOMIC_FETCH_ADD_FULL(a, v) tm_atomic::fetch_and_add_full((tm_atomic::atomic_t*)(a), (uintptr_t)(v))
#define ATOMIC_LOAD_ACQ(a) tm_atomic::load_acquire((tm_atomic::atomic_t*)(a))
#define ATOMIC_LOAD(a) tm_atomic::load((tm_atomic::atomic_t*)(a))
#define ATOMIC_STORE_REL(a, v) tm_atomic::store_release((tm_atomic::atomic_t*)(a), (uintptr_t)(v))
#define ATOMIC_STORE(a, v) tm_atomic::store((tm_atomic::atomic_t*)(a), (uintptr_t)(v))
#define ATOMIC_MB_READ std::atomic_thread_fence(std::memory_order_acquire)
#define ATOMIC_MB_WRITE std::atomic_thread_fence(std::memory_order_release)
#define ATOMIC_MB_FULL std::atomic_thread_fence(std::memory_order_seq_cst)
#endif

#endif