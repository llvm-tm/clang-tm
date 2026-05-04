/**
 * Simple Runtime Test - uses tm_* stubs
 * This test can work with any TM implementation by linking different runtime files
 */

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "NOrec.hpp"

// Runtime stubs - implementation provided by linking with appropriate runtime file
extern "C" {
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();
void tm_begin(); // Returns 1 on success, 0 on abort
void tm_end();   // Returns 1 on success,0 on failure
uint32_t tm_read_i4(volatile uint32_t *addr);
void tm_write_i4(volatile uint32_t *addr, uint32_t val);
}

struct SharedData {
	volatile uint32_t counter;

	SharedData()
	    : counter(0)
	{
	}
};

constexpr int NUM_THREADS = 16;
constexpr int ITERATIONS = 10000;

uint32_t i_array[NUM_THREADS] = {0};

constexpr int BUFFER_SIZE = 65536;
thread_local int buffer_idx = 0;
std::chrono::high_resolution_clock::time_point time_arrays[NUM_THREADS][BUFFER_SIZE];
uint32_t val_array[NUM_THREADS][BUFFER_SIZE] = {0};
uint32_t val_com_array[NUM_THREADS][BUFFER_SIZE] = {0};
uint64_t vs1_array[NUM_THREADS][BUFFER_SIZE] = {0};
uint64_t vs2_array[NUM_THREADS][BUFFER_SIZE] = {0};
uint64_t vs3_array[NUM_THREADS][BUFFER_SIZE] = {0};

SharedData shared_data;

void thread_func(int thread_id)
{
	tm_init_thread();

	volatile uint32_t &i = i_array[thread_id];
	for (i = 0; i < ITERATIONS; ++i) {
		// std::cout << "Thread " << thread_id << " started TX " << i << "\n";
		int committed = 0;
		tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);

		int idx = buffer_idx++ & (BUFFER_SIZE - 1);
		// vs1_array[thread_id][idx] = tinystm::g_locks_wbetl
		//                                 .get((uint64_t)&shared_data.counter & ~7L)
		//                                 .get();

		tm_begin();

		uint32_t val = tm_read_i4(&shared_data.counter);
		// vs2_array[thread_id][idx] = tinystm::g_locks_wbetl
		//                                 .get((uint64_t)&shared_data.counter & ~7L)
		//                                 .get();
		time_arrays[thread_id][idx] = std::chrono::high_resolution_clock::now();
		val_array[thread_id][idx] = ++val;
		tm_write_i4(&shared_data.counter, val);
		// vs3_array[thread_id][idx] = tinystm::g_locks_wbetl
		//                                 .get((uint64_t)&shared_data.counter & ~7L)
		//                                 .get();

		tm_end();
		val_com_array[thread_id][i] = val;

		// if (val < (__atomic_load_n(&i_array[0], __ATOMIC_ACQUIRE) +
		//            __atomic_load_n(&i_array[1], __ATOMIC_ACQUIRE))) {
		// 	printf("INVALID COUNTER!\n");
		// }
		// std::cout << "Thread " << thread_id << " committed TX " << i << "\n";
	}

	tm_exit_thread();
}

int main()
{
	std::cout << "Runtime Simple Test\n";
	std::cout << "====================\n";
	std::cout << "Threads:    " << NUM_THREADS << "\n";
	std::cout << "Iterations: " << ITERATIONS << "\n";
	std::cout << "Expected final counter: " << (NUM_THREADS * ITERATIONS) << "\n\n";

	tm_init();

	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::thread> threads;
	for (int i = 0; i < NUM_THREADS; ++i) {
		threads.emplace_back(thread_func, i);
	}

	for (auto &t : threads) {
		t.join();
	}

	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

	tm_exit();

	std::cout << "\nResults:\n";
	std::cout << "========\n";
	std::cout << "Final counter:  " << shared_data.counter << "\n";
	std::cout << "Expected:       " << (NUM_THREADS * ITERATIONS) << "\n";
	std::cout << "Time elapsed:   " << duration.count() << " ms\n";

	if (shared_data.counter == NUM_THREADS * ITERATIONS) {
		std::cout << "\nTEST PASSED\n";
		return 0;
	} else {
		std::cout << "\nTEST FAILED (counter: " << shared_data.counter
		          << ", expected: " << (NUM_THREADS * ITERATIONS) << ")\n";
		return 1;
	}
}
