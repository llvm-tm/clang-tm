#include "queue_runtime.h"
#include "tm_perf_counters.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

extern "C" void tm_init_thread(void);
extern "C" void tm_exit_thread(void);

// =========================================================================
// Perf counters (gated by -DTM_PERF_COUNTERS, macros in header)
// =========================================================================

#ifdef TM_PERF_COUNTERS
static TmPerfCounters g_caller_perf;

static thread_local TmPerfCounters *g_worker_perf = nullptr;
static TmPerfCounters &worker_perf()
{
    if (!g_worker_perf) {
        static thread_local TmPerfCounters self;
        g_worker_perf = &self;
        tm_perf_registry().all.push_back(&self);
    }
    return *g_worker_perf;
}
#endif

// =========================================================================
// Thread pool
// =========================================================================

class QueueExecutor {
public:
    QueueExecutor(int num_workers, int num_queues)
        : num_queues_(std::max(1, num_queues))
        , num_workers_(std::max(1, num_workers))
        , queues_(num_queues_)
        , queue_mutexes_(num_queues_)
        , queue_cvs_(num_queues_)
    {
        for (int i = 0; i < num_workers_; ++i)
            workers_.emplace_back(&QueueExecutor::workerLoop, this, i);
    }

    ~QueueExecutor() { shutdown(); }

    void enqueue(std::function<void()> task)
    {
        if (shutdown_.load(std::memory_order_relaxed))
            return;
        int qidx = next_queue_.fetch_add(1, std::memory_order_relaxed) % num_queues_;
        TM_PERF_SCOPE(g_caller_perf, push_ns) {
            std::lock_guard<std::mutex> lock(queue_mutexes_[qidx]);
            queues_[qidx].push(std::move(task));
        }
        queue_cvs_[qidx].notify_one();
    }

    void shutdown()
    {
        bool expected = false;
        if (!shutdown_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel))
            return;
        for (auto &cv : queue_cvs_)
            cv.notify_all();
        for (auto &w : workers_) {
            if (w.joinable()) w.join();
        }
    }

private:
    void workerLoop(int)
    {
        tm_init_thread();

#ifdef TM_PERF_COUNTERS
        // Register this worker's counter set (side effect: creates thread_local)
        worker_perf();
#endif

        while (true) {
            std::function<void()> task;
            bool found = false;

            int start_q = next_worker_queue_.fetch_add(1, std::memory_order_relaxed) % num_queues_;
            for (int attempt = 0; attempt < num_queues_; ++attempt) {
                int qidx = (start_q + attempt) % num_queues_;
                std::unique_lock<std::mutex> lock(queue_mutexes_[qidx]);
                if (!queues_[qidx].empty()) {
                    task = std::move(queues_[qidx].front());
                    queues_[qidx].pop();
                    found = true;
                    lock.unlock();
                    break;
                }
                if (attempt == 0) {
                    TM_PERF_SCOPE(worker_perf(), worker_wait_ns) {
                        queue_cvs_[qidx].wait(lock, [this, qidx]() {
                            return shutdown_.load(std::memory_order_relaxed) ||
                                   !queues_[qidx].empty();
                        });
                    }
                    if (!queues_[qidx].empty()) {
                        task = std::move(queues_[qidx].front());
                        queues_[qidx].pop();
                        found = true;
                        lock.unlock();
                        break;
                    }
                    lock.unlock();
                    if (shutdown_.load(std::memory_order_relaxed))
                        goto done;
                }
            }

            if (!found) {
                if (shutdown_.load(std::memory_order_relaxed))
                    goto done;
                TM_PERF_INC(worker_perf(), worker_spins);
                std::this_thread::yield();
                continue;
            }

            TM_PERF_INC(worker_perf(), worker_tasks);
            TM_PERF_SCOPE(worker_perf(), exec_ns) {
                task();
            }
        }
    done:
        tm_exit_thread();
    }

    int num_queues_;
    int num_workers_;
    std::atomic<bool> shutdown_{false};
    std::atomic<int> next_queue_{0};
    std::atomic<int> next_worker_queue_{0};
    std::vector<std::queue<std::function<void()>>> queues_;
    std::vector<std::mutex> queue_mutexes_;
    std::vector<std::condition_variable> queue_cvs_;
    std::vector<std::thread> workers_;
};

static QueueExecutor *g_executor = nullptr;

// =========================================================================
// Thread-local state
// =========================================================================

// Pending-count approach: each caller thread tracks how many of its
// enqueued TXes are still running.  tm_wait_prev_tx() spins until the
// count reaches 0.  This correctly handles both sync (enqueue+wait per TX)
// and async (N enqueues, then one wait at the end).
//
// Each enqueued task captures a pointer to the CALLER's pending counter
// (thread_local static storage — valid for the program's lifetime) and
// decrements it after the TX finishes.

static thread_local std::atomic<int> g_tm_pending_count{0};

#ifdef __cplusplus
extern "C" {
#endif

thread_local int g_tm_queue_active = 0;

void tm_enqueue(void (*fn)(void*), void* args)
{
    TM_PERF_INC(g_caller_perf, enqueue_calls);

    if (!g_tm_queue_active) {
        fn(args);
        TM_PERF_INC(g_caller_perf, inline_execs);
        TM_PERF_END(g_caller_perf);
        return;
    }

    TM_PERF_BEGIN(g_caller_perf);
    TM_PERF_INC(g_caller_perf, queue_execs);

    auto *caller_pending = &g_tm_pending_count;
    caller_pending->fetch_add(1, std::memory_order_relaxed);

    if (!g_executor) {
        fprintf(stderr, "FATAL: tm_enqueue called but queue executor not initialized\n");
        std::abort();
    }

    g_executor->enqueue([fn, args, caller_pending]() {
        fn(args);
        caller_pending->fetch_sub(1, std::memory_order_release);
    });
}

void tm_wait_prev_tx(void)
{
    TM_PERF_INC(g_caller_perf, wait_calls);

    if (!g_tm_queue_active)
        return;

    TM_PERF_SCOPE(g_caller_perf, wait_block_ns) {
        int spins = 0;
        while (g_tm_pending_count.load(std::memory_order_acquire) > 0) {
            if (++spins > 10000)
                std::this_thread::yield();
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
        }
    }

    TM_PERF_END(g_caller_perf);
}

void tm_queue_init(int default_workers, int default_queues)
{
    if (g_executor)
        return;

    const char *env = getenv("THREADS");
    int workers = env ? std::max(1, atoi(env)) : default_workers;
    int queues  = env ? std::max(1, atoi(env)) : default_queues;

    g_executor = new QueueExecutor(workers, queues);
    g_tm_queue_active = 1;

#ifdef TM_PERF_COUNTERS
    std::strncpy(g_caller_perf.name, "caller", 31);
    tm_perf_registry().all.push_back(&g_caller_perf);
#endif
}

void tm_queue_shutdown(void)
{
    if (g_executor) {
        g_executor->shutdown();
        delete g_executor;
        g_executor = nullptr;
    }
    g_tm_queue_active = 0;
    tm_perf_dump();
}

#ifdef __cplusplus
}
#endif
