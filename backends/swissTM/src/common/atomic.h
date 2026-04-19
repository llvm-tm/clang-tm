#ifndef ATOMIC_H_
#define ATOMIC_H_

#include <atomic>
#include <stdint.h>

#include "constants.h"
#include "membar.h"

using aword = uintptr_t;

namespace wlpdstm {

inline bool compare_and_swap_release(uintptr_t *addr, uintptr_t oldval, uintptr_t newval);
}

inline bool atomic_cas_no_barrier(uintptr_t *addr, uintptr_t old_val, uintptr_t new_val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    std::atomic<uintptr_t> expected(old_val);
    return a->compare_exchange_strong(expected, new_val, std::memory_order_relaxed);
}

inline bool atomic_cas_full(uintptr_t *addr, uintptr_t old_val, uintptr_t new_val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    std::atomic<uintptr_t> expected(old_val);
    return a->compare_exchange_strong(expected, new_val, std::memory_order_seq_cst);
}

inline bool atomic_cas_acquire(uintptr_t *addr, uintptr_t old_val, uintptr_t new_val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    std::atomic<uintptr_t> expected(old_val);
    return a->compare_exchange_strong(expected, new_val, std::memory_order_acquire);
}

inline bool atomic_cas_release(uintptr_t *addr, uintptr_t old_val, uintptr_t new_val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    std::atomic<uintptr_t> expected(old_val);
    return a->compare_exchange_strong(expected, new_val, std::memory_order_release);
}

inline void atomic_store_full(uintptr_t *addr, uintptr_t val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    a->store(val, std::memory_order_seq_cst);
}

inline void atomic_store_release(uintptr_t *addr, uintptr_t val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    a->store(val, std::memory_order_release);
}

inline void atomic_store_no_barrier(uintptr_t *addr, uintptr_t val) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    a->store(val, std::memory_order_relaxed);
}

inline uintptr_t atomic_load_acquire(uintptr_t *addr) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    return a->load(std::memory_order_acquire);
}

inline uintptr_t atomic_load_no_barrier(uintptr_t *addr) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    return a->load(std::memory_order_relaxed);
}

inline uintptr_t fetch_and_inc_no_barrier(uintptr_t *addr) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    return a->fetch_add(1, std::memory_order_relaxed);
}

inline uintptr_t fetch_and_inc_full(uintptr_t *addr) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    return a->fetch_add(1, std::memory_order_seq_cst);
}

inline uintptr_t fetch_and_inc_acquire(uintptr_t *addr) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    return a->fetch_add(1, std::memory_order_acquire);
}

inline uintptr_t fetch_and_inc_release(uintptr_t *addr) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    return a->fetch_add(1, std::memory_order_release);
}

inline bool wlpdstm::compare_and_swap_release(uintptr_t *addr, uintptr_t oldval, uintptr_t newval) {
    std::atomic<uintptr_t> *a = reinterpret_cast<std::atomic<uintptr_t>*>(addr);
    std::atomic<uintptr_t> expected(oldval);
    return a->compare_exchange_strong(expected, newval, std::memory_order_release);
}

namespace wlpdstm {

template <typename T, T INITIAL_VALUE>
class AtomicVariable {
    public:
        AtomicVariable() : value(INITIAL_VALUE)
            { }

        void setValue(T newval) {
            atomic_store_no_barrier(&value, newval);
        }

        T getValue() {
            return (T)atomic_load_no_barrier(&value);
        }

        bool exchangeValue(T oldval, T newval) {
            return atomic_cas_no_barrier(&value, oldval, newval);
        }

    protected:
        T value;
};

class CounterOF : protected AtomicVariable<Word, 0> {
    public:
        Word getNext() {
            return fetch_and_inc_no_barrier(&value);
        }

        Word getMax() {
            return getValue();
        }
};

class PaddedSpinTryLock {
    private:
        enum SpinLockState {
            Free,
            Acquired
        };

    public:
        PaddedSpinTryLock() {
            padded_state.state = Free;
        }

        bool try_lock() {
            return atomic_cas_acquire(&(padded_state.state),
                                      Free, Acquired);
        }

        void release() {
            atomic_store_release(&(padded_state.state), Free);
        }

    private:
        union {
            volatile Word state;
            char padding[CACHE_LINE_SIZE_BYTES];
        } padded_state;
};
}

#endif