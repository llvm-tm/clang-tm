#pragma once

#include "tm_api.hpp"
#include "tm_platform.hpp"   // stm::tm_cpu_relax, stm::tm_page_size

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace expli {

// ═══════════════════════════════════════════════════════════════════
// tx_executor.hpp — Execution-strategy abstraction for TM TXes
//
// Separates the retry loop (sigsetjmp/tm_begin/tm_end/siglongjmp)
// from the execution context (which thread, when).
//
// Usage:
//   // inline (default, no change to existing code):
//   TM<int>::transaction([&]() { balance.write(new_bal); });
//
//   // explicit queue mode:
//   QueueExecutor qexec(4, 4);
//   TM<int>::transaction(qexec, [&]() { balance.write(new_bal); });
//
//   // async queue:
//   auto fut = qexec.execute_async([]() {
//       volatile bool done = false;
//       while (!done) { sigsetjmp(...); tm_begin(); body(); tm_end(); done=true; }
//   });
//   fut.wait();
// ═══════════════════════════════════════════════════════════════════


// ── Base class ────────────────────────────────────────────────────
class TxExecutor {
public:
    virtual ~TxExecutor() = default;

    /// Synchronous execution: blocks until the TX commits.
    virtual void execute(std::function<void()> body) = 0;

    /// Asynchronous execution: returns a future.
    virtual std::shared_future<void> execute_async(std::function<void()> body) {
        auto prom = std::make_shared<std::promise<void>>();
        auto fut = prom->get_future().share();
        execute([body = std::move(body), prom]() mutable {
            body();
            prom->set_value();
        });
        return fut;
    }
};


// ── InlineExecutor: runs TX inline (existing behavior) ──────────
class InlineExecutor : public TxExecutor {
public:
    void execute(std::function<void()> body) override {
        volatile bool done = false;
        while (!done) {
            sigsetjmp(tm_jmpbuf, 0);
            tm_nested_call_counter = 1;
            tm_begin();
            body();
            tm_end();
            done = true;
        }
        tm_nested_call_counter = 0;
    }
};


// ── QueueExecutor: enqueues TX to a thread pool ──────────────────
class QueueExecutor : public TxExecutor {
public:
    QueueExecutor(int num_workers = 4, int num_queues = 4)
        : num_queues_(num_queues)
        , shutdown_(false)
        , queues_(num_queues)
        , mutexes_(num_queues)
        , cvs_(num_queues)
    {
        if (num_workers < 1) num_workers = 1;
        if (num_queues_ < 1) num_queues_ = 1;
        for (int i = 0; i < num_workers; ++i)
            workers_.emplace_back(&QueueExecutor::workerLoop, this);
    }

    ~QueueExecutor() { shutdown(); }

    // — sync: enqueue + spin-wait on atomic flag —
    void execute(std::function<void()> body) override {
        auto done = std::make_shared<std::atomic<bool>>(false);
        enqueue([body = std::move(body), done]() {
            body();
            done->store(true, std::memory_order_release);
        });
        while (!done->load(std::memory_order_acquire)) {
            stm::tm_cpu_relax();
        }
    }

    // — async: enqueue, return future —
    std::shared_future<void> execute_async(std::function<void()> body) override {
        auto prom = std::make_shared<std::promise<void>>();
        auto fut = prom->get_future().share();
        enqueue([body = std::move(body), prom]() mutable {
            body();
            prom->set_value();
        });
        return fut;
    }

    void shutdown() {
        bool expected = false;
        if (!shutdown_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;
        for (auto& cv : cvs_)
            cv.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

private:
    void enqueue(std::function<void()> task) {
        int qidx = next_queue_.fetch_add(1, std::memory_order_relaxed) % num_queues_;
        {
            std::lock_guard<std::mutex> lock(mutexes_[qidx]);
            queues_[qidx].push(std::move(task));
        }
        cvs_[qidx].notify_one();
    }

    void workerLoop() {
        tm_init_thread();
        while (true) {
            std::function<void()> task;
            bool found = false;
            int start_q = next_wq_.fetch_add(1, std::memory_order_relaxed) % num_queues_;
            for (int a = 0; a < num_queues_; ++a) {
                int q = (start_q + a) % num_queues_;
                std::unique_lock<std::mutex> lock(mutexes_[q]);
                if (!queues_[q].empty()) {
                    task = std::move(queues_[q].front());
                    queues_[q].pop();
                    found = true;
                    break;
                }
                if (a == 0) {
                    cvs_[q].wait(lock, [this, q]() {
                        return shutdown_.load(std::memory_order_relaxed) ||
                               !queues_[q].empty();
                    });
                    if (!queues_[q].empty()) {
                        task = std::move(queues_[q].front());
                        queues_[q].pop();
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                if (shutdown_.load(std::memory_order_relaxed)) break;
                stm::tm_cpu_relax();
                continue;
            }
            task();
        }
        tm_exit_thread();
    }

    int num_queues_;
    std::atomic<bool> shutdown_;
    std::atomic<int> next_queue_{0};
    std::atomic<int> next_wq_{0};
    std::vector<std::queue<std::function<void()>>> queues_;
    std::vector<std::mutex> mutexes_;
    std::vector<std::condition_variable> cvs_;
    std::vector<std::thread> workers_;
};


// ── Retry-loop helper for use with any executor ──────────────────
// Wraps a user body in the sigsetjmp/tm_begin/body/tm_end retry loop
// and executes it via the given executor.
template<typename F>
void tx_transaction(TxExecutor& exec, F&& body) {
    exec.execute([&body]() {
        volatile bool done = false;
        while (!done) {
            sigsetjmp(tm_jmpbuf, 0);
            tm_nested_call_counter = 1;
            tm_begin();
            body();
            tm_end();
            done = true;
        }
        tm_nested_call_counter = 0;
    });
}


// ── Convenience functions ─────────────────────────────────────────
inline TxExecutor* g_tx_executor = nullptr;

inline TxExecutor& executor() {
    if (g_tx_executor) return *g_tx_executor;
    static InlineExecutor default_exec;
    return default_exec;
}

inline void set_executor(TxExecutor& exec) {
    g_tx_executor = &exec;
}

} // namespace expli
