#ifndef MODERN_ATOMIC_H_
#define MODERN_ATOMIC_H_

#include <atomic>
#include <cstdint>

#define AO_t uintptr_t

namespace modern_atomic {

inline void compiler_barrier() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline bool compare_and_swap(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    return a->compare_exchange_strong(expected, new_val);
}

inline AO_t compare_and_swap_full(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    a->compare_exchange_strong(expected, new_val, std::memory_order_seq_cst);
    return expected.load(std::memory_order_seq_cst);
}

inline AO_t compare_and_swap_acquire(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    a->compare_exchange_strong(expected, new_val, std::memory_order_acquire);
    return expected.load(std::memory_order_acquire);
}

inline AO_t compare_and_swap_release(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    a->compare_exchange_strong(expected, new_val, std::memory_order_release);
    return expected.load(std::memory_order_relaxed);
}

inline AO_t fetch_and_add1(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_add(1, std::memory_order_relaxed);
}

inline AO_t fetch_and_add1_full(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_add(1, std::memory_order_seq_cst);
}

inline AO_t fetch_and_add1_acquire(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_add(1, std::memory_order_acquire);
}

inline AO_t fetch_and_sub1(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_sub(1, std::memory_order_relaxed);
}

inline AO_t fetch_and_sub1_full(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_sub(1, std::memory_order_seq_cst);
}

inline AO_t fetch_and_add_full(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_add(val, std::memory_order_seq_cst);
}

inline void store(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    a->store(val, std::memory_order_relaxed);
}

inline void store_full(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    a->store(val, std::memory_order_seq_cst);
}

inline void store_release(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    a->store(val, std::memory_order_release);
}

inline AO_t load(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->load(std::memory_order_relaxed);
}

inline AO_t load_full(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->load(std::memory_order_seq_cst);
}

inline AO_t load_acquire(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->load(std::memory_order_acquire);
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

#if defined(__GNUC__) || defined(__clang__)
#define AO_COMPILER_BARRIER() __asm__ __volatile__("" : : : "memory")
#else
#define AO_COMPILER_BARRIER() std::atomic_thread_fence(std::memory_order_seq_cst)
#endif

#if defined(__x86_64__) || defined(__amd64__)

inline void modern_atomic_full_fence() {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("mfence" : : : "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}

#endif

#define AO_compiler_barrier() AO_COMPILER_BARRIER()
#define AO_compare_and_swap_full(A, O, N) modern_atomic::compare_and_swap_full((volatile AO_t*)(A), (AO_t)(O), (AO_t)(N))
#define AO_fetch_and_add1_full(A) modern_atomic::fetch_and_add1_full((volatile AO_t*)(A))
#define AO_fetch_and_sub1_full(A) modern_atomic::fetch_and_sub1_full((volatile AO_t*)(A))
#define AO_fetch_and_add_full(A, V) modern_atomic::fetch_and_add_full((volatile AO_t*)(A), (AO_t)(V))
#define AO_load_full(A) modern_atomic::load_full((volatile AO_t*)(A))
#define AO_load_acquire_read(A) modern_atomic::load_acquire((volatile AO_t*)(A))
#define AO_store_full(A, V) modern_atomic::store_full((volatile AO_t*)(A), (AO_t)(V))
#define AO_store_release(A, V) modern_atomic::store_release((volatile AO_t*)(A), (AO_t)(V))
#define AO_nop_full() modern_atomic::nop_full()
#define AO_nop_read() modern_atomic::nop_read()
#define AO_nop_write() modern_atomic::nop_write()

#endif