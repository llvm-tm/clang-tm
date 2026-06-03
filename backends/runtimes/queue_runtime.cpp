#include "queue_runtime.h"
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

// Forward-declare TM thread init/exit functions (C linkage, provided by backend runtime)
extern "C" void tm_init_thread(void);
extern "C" void tm_exit_thread(void);

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
        for (int i = 0; i < num_workers_; ++i) {
            workers_.emplace_back(&QueueExecutor::workerLoop, this, i);
        }
    }

    ~QueueExecutor()
    {
        shutdown();
    }

    void enqueue(std::function<void()> task)
    {
        if (shutdown_.load(std::memory_order_relaxed))
            return;
        // Round-robin queue selection
        int qidx = next_queue_.fetch_add(1, std::memory_order_relaxed) % num_queues_;
        {
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
            return; // already shutting down
        for (auto &cv : queue_cvs_)
            cv.notify_all();
        for (auto &w : workers_) {
            if (w.joinable())
                w.join();
        }
    }

    void wait_all()
    {
        // Busy-wait until all queues are empty and all tasks are done.
        // This is a simplistic approach — for production use a counter.
        while (true) {
            bool all_empty = true;
            for (int i = 0; i < num_queues_; ++i) {
                std::lock_guard<std::mutex> lock(queue_mutexes_[i]);
                if (!queues_[i].empty()) {
                    all_empty = false;
                    break;
                }
            }
            if (all_empty)
                break;
            std::this_thread::yield();
        }
    }

private:
    void workerLoop(int /*worker_id*/)
    {
        tm_init_thread();

        while (true) {
            std::function<void()> task;
            bool found = false;

            // Try to dequeue from assigned queue first, then steal
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
                    // Block on our starting queue
                    queue_cvs_[qidx].wait(lock, [this, qidx]() {
                        return shutdown_.load(std::memory_order_relaxed) ||
                               !queues_[qidx].empty();
                    });
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
                std::this_thread::yield();
                continue;
            }

            task();
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
// Future state (mutex + condition variable + done flag)
// =========================================================================

struct TmFutureState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
};

// =========================================================================
// Thread-local state
// =========================================================================

#ifdef __cplusplus
extern "C" {
#endif

thread_local int   g_tm_queue_active = 0;
thread_local void* g_tm_pending = nullptr;

void tm_enqueue(void (*fn)(void*), void* args)
{
    if (!g_tm_queue_active) {
        // Inline mode: execute synchronously
        fn(args);
        return;
    }

    // Queue mode: create a future for this TX
    auto *state = new TmFutureState();

    // Replace previous pending (if app forgot to wait, leak is accepted)
    g_tm_pending = state;

    if (!g_executor) {
        fprintf(stderr, "FATAL: tm_enqueue called but queue executor not initialized\n");
        std::abort();
    }

    g_executor->enqueue([fn, args, state]() {
        fn(args);  // runs dispatch() → tm_begin/tm_call/tm_commit
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->done = true;
        }
        state->cv.notify_one();
    });
}

void tm_wait_prev_tx(void)
{
    if (!g_tm_queue_active)
        return; // inline mode: TX already completed synchronously

    auto *state = static_cast<TmFutureState*>(g_tm_pending);
    if (!state)
        return; // no pending TX

    g_tm_pending = nullptr; // consume

    std::unique_lock<std::mutex> lock(state->mtx);
    state->cv.wait(lock, [state]() { return state->done; });
    lock.unlock();

    delete state;
}

void tm_queue_init(int default_workers, int default_queues)
{
    if (g_executor)
        return; // already initialized

    const char *env = getenv("THREADS");
    int workers = env ? std::max(1, atoi(env)) : default_workers;
    int queues  = env ? std::max(1, atoi(env)) : default_queues;

    g_executor = new QueueExecutor(workers, queues);
    g_tm_queue_active = 1;
}

void tm_queue_shutdown(void)
{
    if (g_executor) {
        g_executor->shutdown();
        delete g_executor;
        g_executor = nullptr;
    }
    g_tm_queue_active = 0;
}

#ifdef __cplusplus
}
#endif
