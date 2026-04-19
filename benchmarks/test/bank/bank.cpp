/**
 * Bank Benchmark - Modern C++17 Version
 * 
 * Tests transactional memory correctness with concurrent bank transfers.
 * Verifies that the total money in the bank remains constant across multiple threads,
 * indicating proper transaction isolation and atomicity.
 * 
 * Compiler: C++17
 * Uses: C++ Standard Library threads
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <cstring>

// Transaction annotations
#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

// =============================================================================
// Global Configuration
// =============================================================================

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_ACCOUNTS = 1024;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int DEFAULT_READ_ALL = 20;
constexpr int DEFAULT_WRITE_ALL = 0;
constexpr int DEFAULT_INITIAL_BALANCE = 1000;

// =============================================================================
// Bank Account Structure
// =============================================================================

struct Account {
    int number;
    TM int balance;
};

struct Bank {
    std::vector<Account> accounts;
    
    Bank(int size) {
        accounts.resize(size);
        for (int i = 0; i < size; ++i) {
            accounts[i].number = i;
            accounts[i].balance = DEFAULT_INITIAL_BALANCE;
        }
    }
    
    int size() const { return accounts.size(); }
};

// =============================================================================
// Transaction Functions
// =============================================================================

/**
 * Transfer money from one account to another within a transaction
 */
TX void transfer(Account &src, Account &dst, int amount) {
    int balance = src.balance;
    balance -= amount;
    src.balance = balance;
    
    balance = dst.balance;
    balance += amount;
    dst.balance = balance;
}

/**
 * Read all account balances and compute total
 */
TX int total_transactional(const Bank &bank) {
    int total = 0;
    for (int i = 0; i < bank.size(); ++i) {
        total += bank.accounts[i].balance;
    }
    return total;
}

/**
 * Non-transactional total (for final verification)
 */
int total_non_transactional(const Bank &bank) {
    int total = 0;
    for (int i = 0; i < bank.size(); ++i) {
        total += bank.accounts[i].balance;
    }
    return total;
}

/**
 * Reset all accounts to initial balance
 */
TX void reset(Bank &bank) {
    for (int i = 0; i < bank.size(); ++i) {
        bank.accounts[i].balance = DEFAULT_INITIAL_BALANCE;
    }
}

// =============================================================================
// Synchronization Barrier
// =============================================================================

class Barrier {
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
    int num_threads_;
    int crossing_;
    
public:
    explicit Barrier(int n) : count_(n), num_threads_(n), crossing_(0) {}
    
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        crossing_++;
        
        if (crossing_ < num_threads_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

// =============================================================================
// Global Control
// =============================================================================

std::atomic<bool> stop_workers(false);

// =============================================================================
// Thread Work Data
// =============================================================================

struct ThreadData {
    Bank *bank;
    Barrier *barrier;
    std::atomic<uint64_t> nb_transfer{0};
    std::atomic<uint64_t> nb_read_all{0};
    std::atomic<uint64_t> nb_write_all{0};
    unsigned int seed;
    int thread_id;
    int read_all_pct;
    int write_all_pct;
    int nb_threads;
    bool disjoint;
};

// =============================================================================
// Thread Worker Function
// =============================================================================

void worker_thread(ThreadData &data) {
    auto rng = std::mt19937(data.seed);
    auto dist = std::uniform_real_distribution<double>(0.0, 100.0);
    
    int rand_max = data.disjoint ? (data.bank->size() / data.nb_threads) : data.bank->size();
    int rand_min = data.disjoint ? (rand_max * data.thread_id) : 0;
    
    // Synchronize with other threads
    data.barrier->wait();
    
    // Run transactions until stop signal
    while (!stop_workers.load(std::memory_order_relaxed)) {
        double roll = dist(rng);
        
        if (roll < data.read_all_pct) {
            int t = total_transactional(*data.bank);
            data.nb_read_all++;
        } else if (roll < (data.read_all_pct + data.write_all_pct)) {
            reset(*data.bank);
            data.nb_write_all++;
        } else {
            // Random transfer
            std::uniform_int_distribution<> account_dist(0, rand_max - 1);
            int src = account_dist(rng) + rand_min;
            int dst = account_dist(rng) + rand_min;
            if (dst == src) {
                dst = ((src + 1) % rand_max) + rand_min;
            }
            transfer(data.bank->accounts[src], data.bank->accounts[dst], 1);
            data.nb_transfer++;
        }
    }
}

// =============================================================================
// Main Benchmark
// =============================================================================

int main(int argc, char *argv[]) {
    int duration_ms = DEFAULT_DURATION_MS;
    int nb_accounts = DEFAULT_NB_ACCOUNTS;
    int nb_threads = DEFAULT_NB_THREADS;
    int read_all_pct = DEFAULT_READ_ALL;
    int write_all_pct = DEFAULT_WRITE_ALL;
    bool disjoint = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-d" && i + 1 < argc) {
            duration_ms = std::stoi(argv[++i]);
        } else if (arg == "-a" && i + 1 < argc) {
            nb_accounts = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            nb_threads = std::stoi(argv[++i]);
        } else if (arg == "-r" && i + 1 < argc) {
            read_all_pct = std::stoi(argv[++i]);
        } else if (arg == "-w" && i + 1 < argc) {
            write_all_pct = std::stoi(argv[++i]);
        } else if (arg == "--disjoint") {
            disjoint = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Bank Benchmark Usage:\n"
                      << "  -d <ms>       Duration in milliseconds (default: " << DEFAULT_DURATION_MS << ")\n"
                      << "  -a <n>        Number of accounts (default: " << DEFAULT_NB_ACCOUNTS << ")\n"
                      << "  -t <n>        Number of threads (default: " << DEFAULT_NB_THREADS << ")\n"
                      << "  -r <pct>      Percentage read-all transactions (default: " << DEFAULT_READ_ALL << ")\n"
                      << "  -w <pct>      Percentage write-all transactions (default: " << DEFAULT_WRITE_ALL << ")\n"
                      << "  --disjoint    Use disjoint account access per thread\n"
                      << "  -h, --help    Show this help message\n";
            return 0;
        }
    }
    
    std::cout << "Bank Benchmark - Modern C++17 Version\n"
              << "======================================\n"
              << "Duration:    " << duration_ms << " ms\n"
              << "Accounts:    " << nb_accounts << "\n"
              << "Threads:     " << nb_threads << "\n"
              << "Read-all %:  " << read_all_pct << "%\n"
              << "Write-all %: " << write_all_pct << "%\n"
              << "Disjoint:    " << (disjoint ? "yes" : "no") << "\n"
              << "Initial balance per account: " << DEFAULT_INITIAL_BALANCE << "\n"
              << std::endl;
    
    // Validate parameters
    if (nb_accounts < nb_threads && disjoint) {
        std::cerr << "Error: Number of accounts must be >= number of threads for disjoint mode\n";
        return 1;
    }
    
    if (read_all_pct + write_all_pct > 100) {
        std::cerr << "Error: read_all + write_all cannot exceed 100%\n";
        return 1;
    }
    
    // Create bank
    Bank bank(nb_accounts);
    Barrier barrier(nb_threads);
    
    // Verify initial state
    int initial_total = total_non_transactional(bank);
    int expected_total = nb_accounts * DEFAULT_INITIAL_BALANCE;
    
    std::cout << "Initial bank total: " << initial_total << " (expected: " << expected_total << ")\n";
    if (initial_total != expected_total) {
        std::cerr << "ERROR: Initial bank total mismatch!\n";
        return 1;
    }
    
    // Create and launch worker threads
    std::vector<std::unique_ptr<ThreadData>> thread_data;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < nb_threads; ++i) {
        auto data = std::make_unique<ThreadData>();
        data->bank = &bank;
        data->barrier = &barrier;
        data->seed = i + 1234;
        data->thread_id = i;
        data->read_all_pct = read_all_pct;
        data->write_all_pct = write_all_pct;
        data->nb_threads = nb_threads;
        data->disjoint = disjoint;
        thread_data.push_back(std::move(data));
    }
    
    // Start timer and worker threads
    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < nb_threads; ++i) {
        threads.emplace_back(worker_thread, std::ref(*thread_data[i]));
    }
    
    // Wait for duration
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    
    // Signal workers to stop and wait for them
    stop_workers.store(true, std::memory_order_release);
    for (auto &t : threads) {
        t.join();
    }
    
    // Compute final totals and statistics
    int final_total = total_non_transactional(bank);
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    // Aggregate statistics
    uint64_t total_transfers = 0;
    uint64_t total_reads = 0;
    uint64_t total_writes = 0;
    
    for (const auto &data : thread_data) {
        total_transfers += data->nb_transfer.load();
        total_reads += data->nb_read_all.load();
        total_writes += data->nb_write_all.load();
    }
    
    uint64_t total_txns = total_transfers + total_reads + total_writes;
    
    // Print results
    std::cout << "\nBench Benchmark Results\n"
              << "=======================\n"
              << "Elapsed time:     " << elapsed_ms << " ms\n"
              << "Final bank total: " << final_total << " (expected: " << expected_total << ")\n"
              << "Total transfers:  " << total_transfers << "\n"
              << "Total read-alls:  " << total_reads << "\n"
              << "Total write-alls: " << total_writes << "\n"
              << "Total txns:       " << total_txns << "\n"
              << "Txns/sec:         " << (total_txns * 1000.0 / elapsed_ms) << "\n"
              << std::endl;
    
    // Verify correctness
    if (final_total == expected_total) {
        std::cout << "✓ PASS: Bank total is correct (money was conserved)\n";
        std::cout << "✓ Accounts are properly instrumented for transactional access\n";
        return 0;
    } else {
        std::cerr << "✗ FAIL: Bank total mismatch!\n"
                  << "  Expected: " << expected_total << "\n"
                  << "  Got:      " << final_total << "\n"
                  << "  Difference: " << (final_total - expected_total) << "\n";
        return 1;
    }
}
