// Thread barrier for init phases (not inside TX — plain std::atomic).

#pragma once

#include <atomic>
#include <thread>

namespace expli {

class Barrier {
    std::atomic<int> count_;
    std::atomic<int> gen_;
    int n_;
public:
    explicit Barrier(int n) : count_(0), gen_(0), n_(n) {}
    void wait() {
        int gen = gen_.load(std::memory_order_relaxed);
        if (count_.fetch_add(1, std::memory_order_acq_rel) == n_ - 1) {
            count_.store(0, std::memory_order_relaxed);
            gen_.store(gen + 1, std::memory_order_release);
        } else {
            while (gen_.load(std::memory_order_acquire) == gen)
                std::this_thread::yield();
        }
    }
};

} // namespace expli
