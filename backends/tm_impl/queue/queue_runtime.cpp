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
#include <unordered_map>
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
        enqueue(std::move(task), -1);
    }

    void enqueue(std::function<void()> task, int queue_id)
    {
        if (shutdown_.load(std::memory_order_relaxed))
            return;
        int qidx = (queue_id >= 0 && queue_id < num_queues_)
                        ? queue_id
                        : next_queue_.fetch_add(1, std::memory_order_relaxed) % num_queues_;
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
                        queue_cvs_[qidx].wait_for(lock, std::chrono::milliseconds(1),
                            [this, qidx]() {
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
// TX ID and per-TX completion tracking
// =========================================================================

// Global monotonically-increasing transaction ID counter.
static std::atomic<uint64_t> g_next_tx_id{1};

// Completion flag shared between enqueuer and worker.
struct TxCompletion {
    std::atomic<bool> done{false};
};

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

// Per-thread mapping from TX ID to completion record (for tm_wait_tx).
// Records are allocated on enqueue and freed on successful wait.
static thread_local std::unordered_map<uint64_t, TxCompletion*> g_tx_completions;
static thread_local uint64_t g_last_tx_id{0};

// Global (non-TLS) flag visible to all threads.  Unlike g_tm_queue_active
// (thread_local, only set for the enqueuing thread), all workers see this.
// Used by backends (e.g. LeftRight) to skip barrier sync that would deadlock
// when only worker threads run TM transactions.
std::atomic<int> g_tm_queue_global{0};

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

uint64_t tm_enqueue_ex(void (*fn)(void*), void* args, int queue_id)
{
    TM_PERF_INC(g_caller_perf, enqueue_calls);

    if (!g_tm_queue_active) {
        fn(args);
        TM_PERF_INC(g_caller_perf, inline_execs);
        TM_PERF_END(g_caller_perf);
        return 0;
    }

    TM_PERF_BEGIN(g_caller_perf);
    TM_PERF_INC(g_caller_perf, queue_execs);

    auto *completion = new TxCompletion();
    uint64_t tx_id = g_next_tx_id.fetch_add(1, std::memory_order_relaxed);

    g_tx_completions[tx_id] = completion;
    g_last_tx_id = tx_id;

    auto *caller_pending = &g_tm_pending_count;
    caller_pending->fetch_add(1, std::memory_order_relaxed);

    if (!g_executor) {
        fprintf(stderr, "FATAL: tm_enqueue_ex called but queue executor not initialized\n");
        std::abort();
    }

    g_executor->enqueue([fn, args, completion, caller_pending]() {
        fn(args);
        completion->done.store(true, std::memory_order_release);
        caller_pending->fetch_sub(1, std::memory_order_release);
    }, queue_id);

    return tx_id;
}

void tm_wait_tx(uint64_t tx_id)
{
    TM_PERF_INC(g_caller_perf, wait_calls);

    if (!g_tm_queue_active || tx_id == 0)
        return;

    auto it = g_tx_completions.find(tx_id);
    if (it == g_tx_completions.end())
        return;

    TxCompletion *completion = it->second;

    TM_PERF_SCOPE(g_caller_perf, wait_block_ns) {
        int spins = 0;
        while (!completion->done.load(std::memory_order_acquire)) {
            if (++spins > 10000)
                std::this_thread::yield();
        }
    }

    g_tx_completions.erase(it);
    delete completion;

    TM_PERF_END(g_caller_perf);
}

uint64_t tm_last_tx_id(void)
{
    return g_last_tx_id;
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
    g_tm_queue_global.store(1, std::memory_order_release);

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
    g_tm_queue_global.store(0, std::memory_order_release);
    tm_perf_dump();
}

#ifdef __cplusplus
}
#endif
