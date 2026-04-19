#ifndef MODERN_ATOMIC_H_
#define MODERN_ATOMIC_H_

#include <atomic>
#include <cstdint>

typedef uintptr_t AO_t;

inline bool AO_compare_and_swap(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    return a->compare_exchange_strong(expected, new_val);
}

inline AO_t AO_compare_and_swap_full(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    a->compare_exchange_strong(expected, new_val, std::memory_order_seq_cst);
    return expected.load(std::memory_order_seq_cst);
}

inline AO_t AO_compare_and_swap_acquire(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    a->compare_exchange_strong(expected, new_val, std::memory_order_acquire);
    return expected.load(std::memory_order_acquire);
}

inline AO_t AO_compare_and_swap_release(volatile AO_t *addr, AO_t old_val, AO_t new_val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    std::atomic<AO_t> expected(old_val);
    a->compare_exchange_strong(expected, new_val, std::memory_order_release);
    return expected.load(std::memory_order_relaxed);
}

inline AO_t AO_fetch_and_add1(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_add(1, std::memory_order_relaxed);
}

inline AO_t AO_fetch_and_add1_full(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->fetch_add(1, std::memory_order_seq_cst);
}

inline void AO_store_full(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    a->store(val, std::memory_order_seq_cst);
}

inline void AO_store_release(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    a->store(val, std::memory_order_release);
}

inline void AO_store(volatile AO_t *addr, AO_t val) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    a->store(val, std::memory_order_relaxed);
}

inline AO_t AO_load_acquire(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->load(std::memory_order_acquire);
}

inline AO_t AO_load(volatile AO_t *addr) {
    std::atomic<AO_t> *a = reinterpret_cast<std::atomic<AO_t>*>(const_cast<AO_t*>(addr));
    return a->load(std::memory_order_relaxed);
}

#endif