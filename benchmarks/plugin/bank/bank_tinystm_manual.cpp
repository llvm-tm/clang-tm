/**
 * Bank Benchmark - Hand-Instrumented Version (Direct TM stubs, no LLVM plugin)
 *
 * This version directly calls tm_read/tm_write stubs without using the LLVM plugin.
 * Used to verify equivalence with plugin instrumentation.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "tm_vector.hpp"

// #include "tinystm_common.hpp" // -I tm_api_cpp/backends/TinySTM/
#include "tinystm_wbctl.hpp" // -I tm_api_cpp/backends/TinySTM/

constexpr int DEFAULT_DURATION_MS = 5000;
constexpr int DEFAULT_NB_ACCOUNTS = 256;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int DEFAULT_READ_ALL = 20;
constexpr int DEFAULT_WRITE_ALL = 0;
constexpr int DEFAULT_INITIAL_BALANCE = 1000;

struct Account {
	int number;
	uint32_t balance;
};

struct Bank {
	TMSafeVector<Account> accounts;

	Bank(int size)
	{
		accounts.resize(size);
		for (int i = 0; i < size; ++i) {
			accounts[i].number = i;
			accounts[i].balance = DEFAULT_INITIAL_BALANCE;
		}
	}

	int size() const { return (int)accounts.size(); }
};

Bank *bank;

void transfer(int src, int dst, int amount)
{
	sigjmp_buf longjmp_buf;
	tinystm::setjmp(&longjmp_buf);
	int aborted = 0;
	aborted = sigsetjmp(longjmp_buf, 0);
	if (!aborted)
		tinystm::begin();

	Account *a_src = &bank->accounts[src];
	Account *a_dst = &bank->accounts[dst];

	uint32_t balance = tinystm::tm_read_i4(&a_src->balance);
	balance -= amount;
	tinystm::tm_write_i4(&a_src->balance, balance);

	balance = tinystm::tm_read_i4(&a_dst->balance);
	balance += amount;
	tinystm::tm_write_i4(&a_dst->balance, balance);
	tinystm::commit();
}

int total_transactional()
{
	int total = 0;

	sigjmp_buf longjmp_buf;
	tinystm::setjmp(&longjmp_buf);
	int aborted = 0;
	aborted = sigsetjmp(longjmp_buf, 0);

	if (!aborted)
		tinystm::begin();

	for (int i = 0; i < bank->size(); ++i) {
		total += tinystm::tm_read_i4(&bank->accounts[i].balance);
	}

	tinystm::commit();
	return total;
}

int total_non_transactional()
{
	int total = 0;
	for (int i = 0; i < bank->size(); ++i) {
		total += bank->accounts[i].balance;
	}
	return total;
}

void reset()
{
	sigjmp_buf longjmp_buf;
	tinystm::setjmp(&longjmp_buf);
	int aborted = 0;
	aborted = sigsetjmp(longjmp_buf, 0);

	if (!aborted)
		tinystm::begin();

	for (int i = 0; i < bank->size(); ++i) {
		tinystm::tm_write_i4(&bank->accounts[i].balance, DEFAULT_INITIAL_BALANCE);
	}

	tinystm::commit();
}

class Barrier
{
private:
	std::mutex mutex_;
	std::condition_variable cv_;
	int count_;
	int num_threads_;
	int crossing_;

public:
	explicit Barrier(int n)
	    : count_(n),
	      num_threads_(n),
	      crossing_(0)
	{
	}

	void wait()
	{
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

std::atomic<bool> stop_workers(false);

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

void worker_thread(ThreadData &data)
{
	auto rng = std::mt19937(data.seed);
	auto dist = std::uniform_real_distribution<double>(0.0, 100.0);

	int rand_max = data.disjoint ? (data.bank->size() / data.nb_threads)
	                             : data.bank->size();
	int rand_min = data.disjoint ? (rand_max * data.thread_id) : 0;

	tinystm::init_thread();
	data.barrier->wait();

	while (!stop_workers.load(std::memory_order_relaxed)) {
		double roll = dist(rng);

		if (roll < data.read_all_pct) {
			int t = total_transactional();
			data.nb_read_all++;
		} else if (roll < (data.read_all_pct + data.write_all_pct)) {
			reset();
			data.nb_write_all++;
		} else {
			std::uniform_int_distribution<> account_dist(0, rand_max - 1);
			int src = account_dist(rng) + rand_min;
			int dst = account_dist(rng) + rand_min;
			if (dst == src) {
				dst = ((src + 1) % rand_max) + rand_min;
			}
			transfer(src, dst, 1);
			data.nb_transfer++;
		}
	}

	tinystm::exit_thread();
}

int main(int argc, char *argv[])
{
	int duration_ms = DEFAULT_DURATION_MS;
	int nb_accounts = DEFAULT_NB_ACCOUNTS;
	int nb_threads = DEFAULT_NB_THREADS;
	int read_all_pct = DEFAULT_READ_ALL;
	int write_all_pct = DEFAULT_WRITE_ALL;
	bool disjoint = false;

	tinystm::init();

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
		}
	}

	std::cout << "Bank Benchmark - Hand-Instrumented (TinySTM Direct)\n"
	          << "=============================================\n"
	          << "Duration:    " << duration_ms << " ms\n"
	          << "Accounts:    " << nb_accounts << "\n"
	          << "Threads:     " << nb_threads << "\n"
	          << "Read-all %:  " << read_all_pct << "%\n"
	          << "Write-all %: " << write_all_pct << "%\n"
	          << std::endl;

	bank = new Bank(nb_accounts);
	Barrier barrier(nb_threads);

	int initial_total = total_non_transactional();
	int expected_total = nb_accounts * DEFAULT_INITIAL_BALANCE;

	std::cout << "Initial bank total: " << initial_total
	          << " (expected: " << expected_total << ")\n";
	if (initial_total != expected_total) {
		std::cerr << "ERROR: Initial bank total mismatch!\n";
		return 1;
	}

	std::vector<std::unique_ptr<ThreadData>> thread_data;
	std::vector<std::thread> threads;

	for (int i = 0; i < nb_threads; ++i) {
		auto data = std::make_unique<ThreadData>();
		data->bank = bank;
		data->barrier = &barrier;
		data->seed = i + 1234;
		data->thread_id = i;
		data->read_all_pct = read_all_pct;
		data->write_all_pct = write_all_pct;
		data->nb_threads = nb_threads;
		data->disjoint = disjoint;
		thread_data.push_back(std::move(data));
	}

	auto start_time = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < nb_threads; ++i) {
		threads.emplace_back(worker_thread, std::ref(*thread_data[i]));
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

	stop_workers.store(true, std::memory_order_release);
	for (auto &t : threads) {
		t.join();
	}

	int final_total = total_non_transactional();
	auto end_time = std::chrono::high_resolution_clock::now();
	auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time -
	                                                                        start_time)
	                      .count();

	uint64_t total_transfers = 0;
	uint64_t total_reads = 0;
	uint64_t total_writes = 0;

	for (const auto &data : thread_data) {
		total_transfers += data->nb_transfer.load();
		total_reads += data->nb_read_all.load();
		total_writes += data->nb_write_all.load();
	}

	uint64_t total_txns = total_transfers + total_reads + total_writes;

	std::cout << "\nBench Benchmark Results\n"
	          << "=======================\n"
	          << "Elapsed time:     " << elapsed_ms << " ms\n"
	          << "Final bank total: " << final_total << " (expected: " << expected_total
	          << ")\n"
	          << "Total transfers:  " << total_transfers << "\n"
	          << "Total read-alls:  " << total_reads << "\n"
	          << "Total write-alls: " << total_writes << "\n"
	          << "Total txns:       " << total_txns << "\n"
	          << std::endl;

	delete bank;

	tinystm::exit();

	if (final_total == expected_total) {
		std::cout << "PASS: Bank total is correct\n";
		return 0;
	} else {
		std::cerr << "FAIL: Bank total mismatch!\n"
		          << "  Expected: " << expected_total << "\n"
		          << "  Got:      " << final_total << "\n"
		          << "  Difference: " << (final_total - expected_total) << "\n";
		return 1;
	}
}
