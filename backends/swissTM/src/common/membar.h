#ifndef WLPDSTM_MEMBAR_H_
#define WLPDSTM_MEMBAR_H_

#include <atomic>

#ifdef WLPDSTM_X86

inline void membar_full() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void membar_store_load_membar_store_store() {
    membar_full();
}

inline void membar_load_store_membar_store_store() {
    membar_full();
}

inline void membar_store_load() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void membar_store_store() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

#elif defined WLPDSTM_SPARC

inline void membar_store_load_membar_store_store() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void membar_load_store_membar_store_store() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void membar_store_load() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void membar_store_store() {
    std::atomic_thread_fence(std::memory_order_release);
}

#endif

#endif