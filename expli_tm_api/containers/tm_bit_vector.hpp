// TM-friendly bit vector (compact boolean array).
// Flat u64 array, each bit is one boolean; all storage is at a fixed address.
// Used inside tx_retry — individual bits are read/written through TM.

#pragma once

// #include "backends/tm_bit_ops.hpp"  // not present in project
#include <cstdint>
#include <cstring>

// A fixed-capacity bitset.  Capacity is set once at construction and never
// changes.  Bits are stored in a uint64_t[] allocated with "new" (one shot,
// no reallocation).  All operations are intended for use inside tx_retry.
template <int InlineWords = 4>
class TMBitVector {
    uint64_t *words_;
    int num_words_;
    uint64_t inline_  [InlineWords];  // small-case; heap if bigger

public:
    explicit TMBitVector(int nbits = 0)
        : words_(nullptr), num_words_(0) {
        memset(inline_, 0, sizeof(inline_));
        if (nbits > 0) resize(nbits);
    }

    // Reallocate to a new capacity.  Only callable OUTSIDE a TX.
    void resize(int nbits) {
        int nw = (nbits + 63) / 64;
        if (nw <= InlineWords) {
            words_ = inline_;
        } else {
            delete[] words_;          // only if previously heap-allocated
            words_ = new uint64_t[nw]();
        }
        num_words_ = nw;
    }

    ~TMBitVector() {
        if (words_ && words_ != inline_) delete[] words_;
    }

    // Read bit i (thread-safe: each uint64_t load is atomic on x86/arm)
    bool test(int i) const {
        int w = i / 64;
        int b = i % 64;
        return (words_[w] >> b) & 1;
    }

    // Set bit i (atomic store)
    void set(int i) {
        int w = i / 64;
        int b = i % 64;
        __atomic_or_fetch(&words_[w], 1ULL << b, __ATOMIC_SEQ_CST);
    }

    // Clear bit i
    void clear(int i) {
        int w = i / 64;
        int b = i % 64;
        __atomic_and_fetch(&words_[w], ~(1ULL << b), __ATOMIC_SEQ_CST);
    }

    // Bulk clear all bits (for reset between phases)
    void clear_all() {
        for (int w = 0; w < num_words_; w++)
            __atomic_store_n(&words_[w], 0, __ATOMIC_SEQ_CST);
    }

    // Iterate over set bits (pass to a lambda).
    template <typename F>
    void for_each(F f) const {
        for (int w = 0; w < num_words_; w++) {
            uint64_t v = __atomic_load_n(&words_[w], __ATOMIC_SEQ_CST);
            while (v) {
                int bit = __builtin_ctzll(v);
                f(w * 64 + bit);
                v &= v - 1;
            }
        }
    }
};
