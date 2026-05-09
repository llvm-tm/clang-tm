/**
 * Bank Benchmark - Manually Instrumented Version
 *
 * This is the reference implementation with direct TM stubs.
 * Used to verify equivalence with plugin instrumentation.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "tinystm_wbctl.hpp"

constexpr int DEFAULT_DURATION_MS = 1000;
constexpr int DEFAULT_NB_ACCOUNTS = 64;
constexpr int DEFAULT_NB_THREADS = 2;
constexpr int DEFAULT_INITIAL_BALANCE = 1000;

struct Account {
	int number;
	uint32_t balance;
};

struct Bank {
	std::vector<Account> accounts;

	Bank(int size)
	{
		accounts.resize(size);
		for (int i = 0; i < size; ++i) {
			accounts[i].number = i;
			accounts[i].balance = DEFAULT_INITIAL_BALANCE;
		}
	}

	int size() const { return accounts.size(); }
};

Bank *bank;

void transfer(int src, int dst, int amount)
{
	sigjmp_buf longjmp_buf;
	tinystm::setjmp(&longjmp_buf);

	int aborted = sigsetjmp(longjmp_buf, 0);
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
	sigjmp_buf longjmp_buf;
	tinystm::setjmp(&longjmp_buf);

	int aborted = sigsetjmp(longjmp_buf, 0);
	if (!aborted)
		tinystm::begin();

	int total = 0;
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

	int aborted = sigsetjmp(longjmp_buf, 0);
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
	unsigned int seed;
	int thread_id;
	int nb_threads;
	bool disjoint;
};

void worker_thread(ThreadData &data)
{
	tinystm::init_thread();

	auto rng = std::mt19937(data.seed);
	auto dist = std::uniform_real_distribution<double>(0.0, 100.0);

	int rand_max = data.disjoint ? (data.bank->size() / data.nb_threads)
	                             : data.bank->size();
	int rand_min = data.disjoint ? (rand_max * data.thread_id) : 0;

	data.barrier->wait();

	while (!stop_workers.load(std::memory_order_relaxed)) {
		double roll = dist(rng);

		if (roll < 80.0) {
			std::uniform_int_distribution<> account_dist(0, rand_max - 1);
			int src = account_dist(rng) + rand_min;
			int dst = account_dist(rng) + rand_min;
			if (dst == src) {
				dst = ((src + 1) % rand_max) + rand_min;
			}
			transfer(src, dst, 1);
			data.nb_transfer++;
		} else {
			reset();
		}
	}

	tinystm::exit_thread();
}

int main(int argc, char *argv[])
{
	int duration_ms = DEFAULT_DURATION_MS;
	int nb_accounts = DEFAULT_NB_ACCOUNTS;
	int nb_threads = DEFAULT_NB_THREADS;

	tinystm::init();

	bank = new Bank(nb_accounts);
	Barrier barrier(nb_threads);

	int expected_total = nb_accounts * DEFAULT_INITIAL_BALANCE;
	int initial_total = total_non_transactional();

	std::cout << "Bank Manual - Initial total: " << initial_total
	          << " (expected: " << expected_total << ")\n";

	std::vector<std::unique_ptr<ThreadData>> thread_data;
	std::vector<std::thread> threads;

	for (int i = 0; i < nb_threads; ++i) {
		auto data = std::make_unique<ThreadData>();
		data->bank = bank;
		data->barrier = &barrier;
		data->seed = i + 1234;
		data->thread_id = i;
		data->nb_threads = nb_threads;
		data->disjoint = false;
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
	for (const auto &data : thread_data) {
		total_transfers += data->nb_transfer.load();
	}

	std::cout << "\nResults (Manual):\n"
	          << "Elapsed: " << elapsed_ms << " ms\n"
	          << "Final total: " << final_total << " (expected: " << expected_total
	          << ")\n"
	          << "Transfers: " << total_transfers << "\n";

	delete bank;
	tinystm::exit();

	if (final_total == expected_total) {
		std::cout << "PASS\n";
		return 0;
	} else {
		std::cout << "FAIL: " << final_total << " != " << expected_total << "\n";
		return 1;
	}
}
