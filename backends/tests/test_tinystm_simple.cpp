/**
 * Simple TinySTM Test - with print inside transaction
 */

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

extern "C" {
#include "../TinySTM/include/stm.h"
}

struct SharedData {
	int32_t counter = 0;
};

constexpr int NUM_THREADS = 4;
constexpr int ITERATIONS = 2000;

SharedData shared_data;

void thread_func(int thread_id)
{
	stm_init_thread();

	for (int i = 0; i < ITERATIONS; ++i) {
		// fprintf(stderr, "Thread %d iteration %d\n", thread_id, i);
		stm_tx_attr_t a = {{.id = static_cast<unsigned int>(thread_id), .read_only = 0}};
		// sigjmp_buf *e = stm_start(a);
		// if (e != NULL) // In the runtime needs to set the internal buffer using the syntax below
		// 	sigsetjmp(*e, 0);
		// fprintf(stderr, "Thread %d calling stm_start\n", thread_id);
		jmp_buf e;
		stm_start(a);
		setjmp(e);                     // sets the jump to AFTER TinySTM begin!
		stm_set_env((sigjmp_buf *)&e); // copies the longjmp buffer into TinySTM
		stm_word_t val = stm_load((volatile stm_word_t *)&shared_data.counter);
		val++;
		stm_store((volatile stm_word_t *)&shared_data.counter, val);
		stm_commit();
		// fprintf(stderr, "After commit on thread %d\n", thread_id);
	}

	stm_exit_thread();
}

int main()
{
	std::cout << "TinySTM Simple STM Test\n";
	std::cout << "======================\n";
	std::cout << "Threads:    " << NUM_THREADS << "\n";
	std::cout << "Iterations: " << ITERATIONS << "\n";
	std::cout << "Expected final counter: " << (NUM_THREADS * ITERATIONS) << "\n\n";

	stm_init();

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

	stm_exit();

	std::cout << "\nResults:\n";
	std::cout << "========\n";
	std::cout << "Final counter:  " << shared_data.counter << "\n";
	std::cout << "Expected:       " << (NUM_THREADS * ITERATIONS) << "\n";
	std::cout << "Time elapsed:   " << duration.count() << " ms\n";

	if (shared_data.counter == NUM_THREADS * ITERATIONS) {
		std::cout << "\nTEST PASSED\n";
		return 0;
	} else {
		std::cout << "\nTEST FAILED\n";
		return 1;
	}
}
