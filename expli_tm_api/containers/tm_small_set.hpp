// TM-friendly small set (fixed-capacity, linear scan).
// Holds expli::TM<T> values internally so all accesses inside tx_retry
// go through the TM runtime.  -1 sentinel marks empty slots.
// Efficient for very small sets (≤ 16 elements).

#pragma once

#include "../tm_api.hpp"
#include <cstddef>
#include <vector>

template <typename T, int MaxSize = 16>
class TMSmallSet {
    expli::TM<T> data_[MaxSize];
    expli::TM<int> count_;

public:
    TMSmallSet() : count_() {
        count_.poke(0);
        for (int i = 0; i < MaxSize; i++) data_[i].poke(T(-1));
    }

    // ── TM methods (call only inside tx_retry) ────────────────
    int count() const { return count_.read(); }
    bool empty() const { return count_.read() == 0; }
    static constexpr int max_size() { return MaxSize; }

    bool contains(T val) const {
        int n = count_.read();
        for (int i = 0; i < n; i++)
            if (data_[i].read() == val) return true;
        return false;
    }

    void insert(T val) {
        if (contains(val)) return;
        int n = count_.read();
        if (n < MaxSize) {
            data_[n].write(val);
            count_.write(n + 1);
        }
    }

    void erase(T val) {
        int n = count_.read();
        for (int i = 0; i < n; i++) {
            if (data_[i].read() == val) {
                T last = data_[n - 1].read();
                data_[i].write(last);
                data_[n - 1].write(T(-1));
                count_.write(n - 1);
                return;
            }
        }
    }

    // Direct indexed access (TM).
    T get(int i) const { return data_[i].read(); }

    // Collect all elements into a vector (TM reads).
    void collect(std::vector<T>& out) const {
        int n = count_.read();
        out.clear();
        for (int i = 0; i < n; i++)
            out.push_back(data_[i].read());
    }

    int index_of(T val) const {
        int n = count_.read();
        for (int i = 0; i < n; i++)
            if (data_[i].read() == val) return i;
        return -1;
    }

    template <typename F>
    void for_each(F f) const {
        int n = count_.read();
        for (int i = 0; i < n; i++)
            f(i, data_[i].read());
    }

    // ── Non-TM setup methods (call only outside any TX) ───────
    void setup_clear() {
        for (int i = 0; i < MaxSize; i++) data_[i].poke(T(-1));
        count_.poke(0);
    }

    void setup_insert(T val) {
        int n = count_.peek();
        if (n < MaxSize) {
            data_[n].poke(val);
            count_.poke(n + 1);
        }
    }

    int setup_count() const { return count_.peek(); }
    bool setup_contains(T val) const {
        int n = count_.peek();
        for (int i = 0; i < n; i++)
            if (data_[i].peek() == val) return true;
        return false;
    }
    T setup_get(int i) const { return data_[i].peek(); }
};
