// Minimal self-contained vector to isolate the stack-write-via-tm_write_ptr bug.
#include "../../backends/tm_safe_map.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <utility>

#include "tm_test_common.hpp"

// Minimal vector with raw pointers (no split_buffer, no _ConstructTransaction)
template <typename T>
struct SimpleVec {
    T *begin_;
    T *end_;
    T *cap_;

    SimpleVec() : begin_(nullptr), end_(nullptr), cap_(nullptr) {}

    size_t size() const { return end_ - begin_; }
    size_t capacity() const { return cap_ - begin_; }

    void push_back(T val) {
        if (end_ != cap_) {
            // Fast path
            *end_ = std::move(val);
            ++end_;
        } else {
            // Slow path: allocate new buffer
            size_t old_sz  = size();
            size_t new_cap = old_sz ? old_sz * 2 : 4;
            T *nb = (T *)::operator new(new_cap * sizeof(T));
            for (size_t i = 0; i < old_sz; i++)
                new (nb + i) T(std::move(begin_[i]));
            if (begin_) {
                for (size_t i = 0; i < old_sz; i++)
                    begin_[i].~T();
                ::operator delete(begin_);
            }
            begin_ = nb;
            end_   = nb + old_sz;
            cap_   = nb + new_cap;
            // Now we have capacity — construct at end
            *end_ = std::move(val);
            ++end_;
        }
    }
};

TM SimpleVec<int64_t> g_sv;
TM std::atomic<int64_t> g_sv_sum{0};

const int ITEMS_PER_TX = 100;

TX void simple_vec_tx(int64_t base)
{
    for (int i = 0; i < ITEMS_PER_TX; i++)
        g_sv.push_back(base + i);
    g_sv_sum.fetch_add(ITEMS_PER_TX);
}

THREAD void sv_worker(int id)
{
    (void)id;
    for (int tx_num = 0; tx_num < 50; tx_num++) {
        int64_t base = (int64_t)(tx_num * 1000);
        simple_vec_tx(base);
    }
}

MAIN int main()
{
    printf("Simple vector test\n");
    printf("==================\n");

    std::thread t(sv_worker, 0);
    t.join();

    printf("g_sv.size() = %zu\n", g_sv.size());
    printf("g_sv_sum    = %lld\n", (long long)g_sv_sum.load());

    // Verify values
    bool ok = true;
    for (size_t i = 0; i < g_sv.size(); i++) {
        int64_t expected = (int64_t)((i / ITEMS_PER_TX) * 1000) + (int64_t)(i % ITEMS_PER_TX);
        if (g_sv.begin_[i] != expected) {
            printf("  FAIL at [%zu]: got %lld expected %lld\n",
                   i, (long long)g_sv.begin_[i], (long long)expected);
            ok = false;
            break;
        }
    }

    if (ok)
        printf("\n  Result: PASS\n");
    else
        printf("\n  Result: FAIL\n");

    return ok ? 0 : 1;
}
