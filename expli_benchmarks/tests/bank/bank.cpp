#include "expli_tm_api/tm_api.hpp"
#include "expli_tm_api/tx_executor.hpp"
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

    while (!g_stop.load()) {
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
        else if (!strcmp(argv[i], "--disjoint")) disjoint = true;
        else if (!strcmp(argv[i], "--queue")) queue_mode = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: bank [-d ms] [-a n] [-t n] [-r pct] [-w pct] [--disjoint] [--queue]\n");
            return 0;
        }
    }

    printf("Bank Benchmark — Explicit TM API\n");
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
    Barrier barrier(nb_threads);

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

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    g_stop.store(true);
    for (int i = 0; i < nb_threads; ++i) threads[i].join();

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
    printf("  Total txns: %llu  Txns/sec: %.0f\n",
           (unsigned long long)total_txns, total_txns*1000.0/elapsed);

    delete g_bank;
    if (qexec) {
        g_executor = nullptr;
        delete qexec;
    }
    expli::TM<int>::exit();

    if (final_total == expected_total) {
        printf("PASS: Money conserved\n");
        return 0;
    }
    fprintf(stderr, "FAIL: Money %s by %d\n",
            final_total > expected_total ? "created" : "destroyed",
            final_total > expected_total ? final_total - expected_total : expected_total - final_total);
    return 1;
}
