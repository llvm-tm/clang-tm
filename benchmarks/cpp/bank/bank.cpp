#include "expli_tm_api/tm_api.hpp"
#include "expli_tm_api/tx_executor.hpp"
#include "../gem5_roi.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

// ── Configuration ───────────────────────────────────────────
constexpr int DEFAULT_DURATION_MS  = 10000;
constexpr int DEFAULT_NB_ACCOUNTS  = 1024;
constexpr int DEFAULT_NB_THREADS   = 4;
constexpr int DEFAULT_READ_ALL     = 20;
constexpr int DEFAULT_WRITE_ALL    = 0;
constexpr int DEFAULT_INITIAL_BALANCE = 1000;

// ── Account & Bank ──────────────────────────────────────────
struct Account {
    int number;
    expli::TM<int> balance;
};

struct Bank {
    expli::vector<Account> accounts;

    Bank(int size) {
        accounts.resize(size);
        for (int i = 0; i < size; ++i) {
            accounts[i].number = i;
            accounts[i].balance.poke(DEFAULT_INITIAL_BALANCE);
        }
    }
    int size() const { return (int)accounts.size(); }
};

Bank *g_bank;   // set once in main, never modified

// ── Executor (optional, set by --queue) ─────────────────────
static expli::TxExecutor *g_executor = nullptr;

// Dispatch to either the inline TM::transaction or the executor
template<typename F>
void run_tx(F&& body) {
    if (g_executor) {
        tx_transaction(*g_executor, std::forward<F>(body));
    } else {
        expli::TM<int>::transaction(std::forward<F>(body));
    }
}

// ── TX functions ────────────────────────────────────────────
void transfer(int src, int dst, int amount) {
    run_tx([&]() {
        Account *a_src = &g_bank->accounts[src];
        Account *a_dst = &g_bank->accounts[dst];

        int bal = a_src->balance.read();
        bal -= amount;
        a_src->balance.write(bal);

        bal = a_dst->balance.read();
        bal += amount;
        a_dst->balance.write(bal);
    });
}

int total_transactional() {
    int total = 0;
    run_tx([&]() {
        total = 0;
        for (int i = 0; i < g_bank->size(); ++i)
            total += g_bank->accounts[i].balance.read();
    });
    return total;
}

void reset() {
    run_tx([&]() {
        for (int i = 0; i < g_bank->size(); ++i)
            g_bank->accounts[i].balance.write(DEFAULT_INITIAL_BALANCE);
    });
}

int total_non_transactional() {
    // Read directly without TM (only called outside any TX)
    int total = 0;
    for (int i = 0; i < g_bank->size(); ++i)
        total += g_bank->accounts[i].balance.peek();
    return total;
}

// ── Barrier ─────────────────────────────────────────────────
class Barrier {
    std::mutex mtx_;
    std::condition_variable cv_;
    int count_;
    int crossing_{0};
public:
    explicit Barrier(int n) : count_(n) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        if (++crossing_ < count_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

// ── Control ─────────────────────────────────────────────────
std::atomic<bool> g_stop{false};
// Start gate: main releases workers only after it has armed the ROI
// (m5_reset_stats in gem5 builds), so stats cover exactly the TX phase.
std::atomic<bool> g_start{false};
// Optional fixed-size workload: stop after this many loop iterations
// (transactions) across all workers (0 = unlimited, time-based).
// Preferred for cycle-accurate simulation (deterministic, bounded).
static uint64_t g_max_txns = 0;
static std::atomic<uint64_t> g_txn_ticket{0};

// ── Thread Data ─────────────────────────────────────────────
struct ThreadData {
    Barrier *barrier;
    std::atomic<uint64_t> nb_transfer;
    std::atomic<uint64_t> nb_read_all;
    std::atomic<uint64_t> nb_write_all;
    unsigned int seed;
    int thread_id, read_all_pct, write_all_pct, nb_threads;
    bool disjoint;

    ThreadData(Barrier *b, unsigned s, int tid, int ra, int wa, int nt, bool dis)
        : barrier(b), nb_transfer(0), nb_read_all(0), nb_write_all(0),
          seed(s), thread_id(tid), read_all_pct(ra), write_all_pct(wa),
          nb_threads(nt), disjoint(dis) {}
};

// ── Worker ──────────────────────────────────────────────────
void worker_thread(ThreadData &data) {
    expli::TM<int>::thread_init();

    auto rng = std::mt19937(data.seed);

    int rand_max = data.disjoint ? (g_bank->size() / data.nb_threads) : g_bank->size();
    int rand_min = data.disjoint ? (rand_max * data.thread_id) : 0;

    data.barrier->wait();
    while (!g_start.load(std::memory_order_acquire))
        std::this_thread::yield();

    while (!g_stop.load()) {
        if (g_max_txns &&
            g_txn_ticket.fetch_add(1, std::memory_order_relaxed) >= g_max_txns) {
            g_stop.store(true); // quota exhausted: stop all workers
            break;
        }
        double roll = std::uniform_real_distribution<double>(0, 100.0)(rng);
        if (roll < data.read_all_pct) {
            total_transactional();
            data.nb_read_all.fetch_add(1);
        } else if (roll < data.read_all_pct + data.write_all_pct) {
            reset();
            data.nb_write_all.fetch_add(1);
        } else {
            std::uniform_int_distribution<> ad(0, rand_max - 1);
            int src = ad(rng) + rand_min;
            int dst = ad(rng) + rand_min;
            if (dst == src)
                dst = ((src + 1) % rand_max) + rand_min;
            transfer(src, dst, 1);
            data.nb_transfer.fetch_add(1);
        }
    }

    expli::TM<int>::thread_exit();
}

// ── Main ────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    // Line-buffer stdout so progress is visible under simulators (gem5 SE
    // redirects guest stdout to a file; full buffering hides it until exit).
    setvbuf(stdout, NULL, _IOLBF, 0);
    int duration_ms = DEFAULT_DURATION_MS;
    int nb_accounts = DEFAULT_NB_ACCOUNTS;
    int nb_threads  = DEFAULT_NB_THREADS;
    int read_all_pct  = DEFAULT_READ_ALL;
    int write_all_pct = DEFAULT_WRITE_ALL;
    bool disjoint = false;
    bool queue_mode = false;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "-d") && i+1 < argc) duration_ms  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i+1 < argc) nb_accounts  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i+1 < argc) nb_threads   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i+1 < argc) read_all_pct = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i+1 < argc) write_all_pct = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i+1 < argc) g_max_txns   = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--disjoint")) disjoint = true;
        else if (!strcmp(argv[i], "--queue")) queue_mode = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: bank [-d ms] [-a n] [-t n] [-r pct] [-w pct] [-n txns] [--disjoint] [--queue]\n");
            printf("  -n txns   stop after this many transactions total (0 = run for -d ms)\n");
            return 0;
        }
    }

    printf("Bank Benchmark — Explicit TM API\n");
    if (g_max_txns)
        printf("Quota: %llu txns  Accounts: %d  Threads: %d\n",
               (unsigned long long)g_max_txns, nb_accounts, nb_threads);
    else
        printf("Duration: %d ms  Accounts: %d  Threads: %d\n", duration_ms, nb_accounts, nb_threads);

    if (nb_accounts < nb_threads && disjoint) {
        fprintf(stderr, "Error: accounts < threads for disjoint mode\n");
        return 1;
    }

    // Queue executor (if --queue)
    expli::QueueExecutor *qexec = nullptr;
    if (queue_mode) {
        qexec = new expli::QueueExecutor(nb_threads, nb_threads);
        g_executor = qexec;
        printf("Mode: queue (%d workers, %d queues)\n", nb_threads, nb_threads);
    } else {
        printf("Mode: inline\n");
    }

    expli::TM<int>::init();

    g_bank = new Bank(nb_accounts);
    Barrier barrier(nb_threads + 1); // workers + main (start gate)

    int expected_total = nb_accounts * DEFAULT_INITIAL_BALANCE;
    int initial_total  = total_non_transactional();
    printf("Initial total: %d  Expected: %d\n", initial_total, expected_total);
    if (initial_total != expected_total) { fprintf(stderr, "ERROR\n"); return 1; }

    void *td_mem = operator new[](nb_threads * sizeof(ThreadData));
    ThreadData *td = (ThreadData*)td_mem;
    for (int i = 0; i < nb_threads; ++i)
        new (&td[i]) ThreadData(&barrier, (unsigned)(i+1234), i, read_all_pct, write_all_pct, nb_threads, disjoint);

    auto start = std::chrono::high_resolution_clock::now();
    void *thr_mem = operator new[](nb_threads * sizeof(std::thread));
    std::thread *threads = (std::thread*)thr_mem;
    for (int i = 0; i < nb_threads; ++i)
        new (&threads[i]) std::thread(worker_thread, std::ref(td[i]));

    // All workers are parked on the start gate: arm the ROI, then release.
    barrier.wait();
    ROI_RESET_STATS();
    g_start.store(true, std::memory_order_release);

    if (g_max_txns) {
        // Bounded mode: poll for quota exhaustion with short sleeps so the
        // main core idles (sleep syscall) instead of spinning during the ROI.
        while (!g_stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
        g_stop.store(true);
    }
    for (int i = 0; i < nb_threads; ++i) threads[i].join();
    ROI_DUMP_STATS();

    int final_total = total_non_transactional();
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t txfer=0, reads=0, writes=0;
    for (int i = 0; i < nb_threads; ++i) {
        txfer += td[i].nb_transfer.load();
        reads += td[i].nb_read_all.load();
        writes+= td[i].nb_write_all.load();
    }

    for (int i = 0; i < nb_threads; ++i) {
        td[i].~ThreadData();
        threads[i].~thread();
    }
    operator delete[](td_mem);
    operator delete[](thr_mem);
    uint64_t total_txns = txfer + reads + writes;

    printf("\nResults:\n");
    printf("  Elapsed: %lld ms\n", (long long)elapsed);
    printf("  Final total: %d  Expected: %d\n", final_total, expected_total);
    printf("  Transfers: %llu  Read-all: %llu  Write-all: %llu\n",
           (unsigned long long)txfer, (unsigned long long)reads, (unsigned long long)writes);
    // Integer throughput: avoids musl fmt_fp's large stack frames, which
    // trip an unmapped-stack panic under gem5 SE (and inf when elapsed==0).
    if (elapsed > 0) {
        printf("  Total txns: %llu  Txns/sec: %llu\n",
               (unsigned long long)total_txns,
               (unsigned long long)(total_txns * 1000ULL / (uint64_t)elapsed));
    } else {
        printf("  Total txns: %llu  Txns/sec: N/A (elapsed < 1 ms)\n",
               (unsigned long long)total_txns);
    }

    delete g_bank;
    if (qexec) {
        g_executor = nullptr;
        delete qexec;
    }
    expli::TM<int>::exit();

    if (final_total == expected_total) {
        printf("PASS: Money conserved\n");
        fflush(stdout);
        ROI_EXIT(0); // gem5: end simulation now, skip OS shutdown
        return 0;
    }
    fprintf(stderr, "FAIL: Money %s by %d\n",
            final_total > expected_total ? "created" : "destroyed",
            final_total > expected_total ? final_total - expected_total : expected_total - final_total);
    fflush(stderr);
    ROI_EXIT(1);
    return 1;
}
