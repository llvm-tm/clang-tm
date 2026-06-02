// TM-friendly binary heap priority queue.
// Fixed capacity (set at construction), no reallocation during TM.
// All element accesses go through expli::TM<T> so TM tracks reads/writes.

#pragma once

#include "../tm_api.hpp"
#include <algorithm>
#include <cstddef>

template <typename T, int MaxSize = 4096>
class TMHeap {
    expli::TM<T> data_[MaxSize];
    expli::TM<int> size_;

public:
    TMHeap() : size_(0) {}

    int size() const { return size_.read(); }
    bool empty() const { return size_.read() == 0; }
    static constexpr int capacity() { return MaxSize; }

    void clear() { size_.write(0); }

    // Push a value (inside tx_retry).
    void push(T val) {
        int n = size_.read();
        if (n >= MaxSize) return;
        int i = n;
        size_.write(n + 1);
        data_[i].write(val);
        // Sift up
        while (i > 0) {
            int p = (i - 1) / 2;
            T pv = data_[p].read();
            T cv = data_[i].read();
            if (cv < pv) {
                data_[p].write(cv);
                data_[i].write(pv);
                i = p;
            } else break;
        }
    }

    // Pop the minimum (inside tx_retry).
    void pop() {
        int n = size_.read();
        if (n == 0) return;
        T last = data_[n - 1].read();
        size_.write(n - 1);
        n--;
        data_[0].write(last);
        // Sift down
        int i = 0;
        while (true) {
            int smallest = i;
            T iv = data_[i].read();
            int l = 2 * i + 1;
            int r = 2 * i + 2;
            if (l < n) {
                T lv = data_[l].read();
                if (lv < iv) smallest = l;
            }
            T sv = data_[smallest].read();
            if (r < n) {
                T rv = data_[r].read();
                if (rv < sv) { smallest = r; sv = data_[smallest].read(); }
            }
            if (smallest != i) {
                data_[smallest].write(iv);
                data_[i].write(sv);
                i = smallest;
            } else break;
        }
    }

    // Peek at the minimum (inside tx_retry).
    T top() const {
        return data_[0].read();
    }
};
