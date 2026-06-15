/**
 * EigenBench Implementation
 *
 * Based on: Eigenbench: A Simple Exploration Tool for Orthogonal TM Characteristics
 * Authors: S. Hong, T. Oguntebi, J. Casper, N. Bronson, C. Kozyrakis, K. Olukotun
 * Published: IISWC 2010
 *
 * Specification: https://ieeexplore.ieee.org/document/5648812
 *
 * 8 Orthogonal TM Characteristics:
 * 1. Concurrency - number of parallel transactions
 * 2. Transaction Length - number of operations per transaction (R1+W1)
 * 3. Working Set Size - amount of data accessed (A2, A3 array sizes)
 * 4. Temporal Locality - data reuse patterns (locality parameter)
 * 5. Pollution - cache behavior effects (random vs sequential access)
 * 6. Contention - probability of conflicts on shared data
 * 7. Predominance - ratio of reads to writes
 * 8. Density - spatial locality of accesses
 *
 * Test Types:
 * - R1/W1: Shared array contention test
 * - R2/W2: Thread-local array (false sharing test)
 * - R3_i: Inner loop transactional ops
 * - R3_o: Outer loop non-transactional ops
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include "tm_vector.hpp"

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int MAX_THREADS = 64;
constexpr int MAX_ARRAY = 100000;

struct Config {
	int threads;
	int duration;
	int R1;
	int W1;
	int R2;
	int W2;
	int A1;
	int A2;
	int A3;
	int iterations;
	double contention;
	double locality;
	double density;
	bool enable_R2;
	bool enable_R3;
	int mode;
};

TM long g_array1[MAX_THREADS];
TM long g_array2[MAX_THREADS * MAX_ARRAY];
TM long g_array3[MAX_THREADS * MAX_ARRAY];

TM long g_shared_counter = 0;

Config g_config;

struct ThreadLocalData {
	TMSafeVector<int> indices_R1;
	TMSafeVector<int> indices_W1;
	TMSafeVector<int> indices_R2;
	TMSafeVector<int> indices_W2;
	TMSafeVector<int> indices_A3;
	std::mt19937 *rng;
};

void generate_access_patterns(ThreadLocalData &td, int thread_id)
{
	std::uniform_int_distribution<int> dist(0, g_config.A1 - 1);
	std::uniform_int_distribution<int> dist2(0, g_config.A2 - 1);
	std::uniform_int_distribution<int> dist3(0, g_config.A3 - 1);

	for (int i = 0; i < g_config.R1; i++) {
		if (g_config.contention < 0.5) {
			td.indices_R1.push_back(dist(*td.rng));
		} else {
			td.indices_R1.push_back(thread_id % g_config.A1);
		}
	}

	for (int i = 0; i < g_config.W1; i++) {
		if (g_config.contention < 0.5) {
			td.indices_W1.push_back(dist(*td.rng));
		} else {
			td.indices_W1.push_back(thread_id % g_config.A1);
		}
	}

	for (int i = 0; i < g_config.R2; i++) {
		int base = thread_id * g_config.A2;
		if (g_config.locality > 0.8) {
			td.indices_R2.push_back(base + i % g_config.A2);
		} else if (g_config.locality > 0.5) {
			td.indices_R2.push_back(base + dist2(*td.rng));
		} else {
			td.indices_R2.push_back(base + (i * 137) % g_config.A2);
		}
	}

	for (int i = 0; i < g_config.W2; i++) {
		int base = thread_id * g_config.A2;
		if (g_config.density > 0.8) {
			td.indices_W2.push_back(base + i);
		} else if (g_config.density > 0.5) {
			td.indices_W2.push_back(base + dist2(*td.rng));
		} else {
			td.indices_W2.push_back(base + (i * 173) % g_config.A2);
		}
	}

	for (int i = 0; i < g_config.iterations; i++) {
		if (g_config.locality > 0.8) {
			td.indices_A3.push_back(i % g_config.A3);
		} else {
			td.indices_A3.push_back(dist3(*td.rng));
		}
	}
}

TM long read_shared_array1(int idx) { return g_array1[idx]; }

TM void write_shared_array1(int idx, long val) { g_array1[idx] = val; }

TM long read_thread_array2(int idx) { return g_array2[idx]; }

TM void write_thread_array2(int idx, long val) { g_array2[idx] = val; }

TM long read_thread_array3(int idx) { return g_array3[idx]; }

TM void write_thread_array3(int idx, long val) { g_array3[idx] = val; }

TX long txn_read_shared(int idx) { return read_shared_array1(idx); }

TX void txn_write_shared(int idx, long val) { write_shared_array1(idx, val); }

TX long txn_read_local(int idx) { return read_thread_array2(idx); }

TX void txn_write_local(int idx, long val) { write_thread_array2(idx, val); }

std::atomic<bool> done{false};
std::atomic<uint64_t> total_ops{0};
std::atomic<uint64_t> total_reads{0};
std::atomic<uint64_t> total_writes{0};
std::atomic<uint64_t> total_tx{0};

struct WorkerData {
	int id;
	int loops;
	ThreadLocalData td;
};

THREAD void worker(WorkerData *w)
{
	int t = w->id;
	long val = 1;
	int R1 = g_config.R1;
	int W1 = g_config.W1;
	int R2 = g_config.R2;
	int W2 = g_config.W2;

	for (int i = 0; i < w->loops && !done.load(std::memory_order_relaxed); i++) {
		if (g_config.mode == 0 || g_config.mode == 1) {
			TX long sum = 0;
			for (int j = 0; j < R1; j++) {
				sum += txn_read_shared(w->td.indices_R1[j % w->td.indices_R1.size()]);
			}
			for (int j = 0; j < W1; j++) {
				txn_write_shared(w->td.indices_W1[j % w->td.indices_W1.size()], sum + j);
			}
			total_tx.fetch_add(1, std::memory_order_relaxed);
			val = sum;

			if (g_config.enable_R2 && R2 > 0) {
				for (int j = 0; j < R2; j++) {
					val += txn_read_local(t * g_config.A2 +
					                      w->td.indices_R2[j % w->td.indices_R2.size()]);
				}
				for (int j = 0; j < W2; j++) {
					txn_write_local(t * g_config.A2 +
					                    w->td.indices_W2[j % w->td.indices_W2.size()],
					                val);
				}
			}
		}

		if (g_config.enable_R3 && g_config.mode != 1) {
			for (int j = 0; j < (int)w->td.indices_A3.size(); j++) {
				int idx = t * g_config.A3 + w->td.indices_A3[j];
				val += read_thread_array3(idx);
				write_thread_array3(idx, val);
			}
		}

		total_ops.fetch_add(1, std::memory_order_relaxed);
		total_reads.fetch_add(R1 + R2, std::memory_order_relaxed);
		total_writes.fetch_add(W1 + W2, std::memory_order_relaxed);
	}
}

void print_usage(const char *prog)
{
	std::cout << "========= EigenBench =========\n";
	std::cout << "==============================\n";
	std::cout << "Usage: " << prog << " [options]\n\n";
	std::cout << "Options:\n";
	std::cout << "  -t <n>        Number of threads (default: 4)\n";
	std::cout << "  -d <ms>       Duration in ms (default: 10000)\n";
	std::cout << "  --r1 <n>      R1: read operations on shared array (default: 10)\n";
	std::cout << "  --w1 <n>      W1: write operations on shared array (default: 10)\n";
	std::cout << "  --r2 <n>      R2: read ops on thread-local array (default: 10)\n";
	std::cout << "  --w2 <n>      W2: write ops on thread-local array (default: 10)\n";
	std::cout << "  --a1 <n>      A1: shared array size (default: 100)\n";
	std::cout << "  --a2 <n>      A2: per-thread array size (default: 10000)\n";
	std::cout << "  --a3 <n>      A3: R3 array size (default: 10000)\n";
	std::cout << "  --contention <0-1>  Contention level (default: 0.5)\n";
	std::cout << "  --locality <0-1>    Temporal locality (default: 0.5)\n";
	std::cout << "  --density <0-1>    Spatial density (default: 0.5)\n";
	std::cout << "  --enable-r2         Enable R2/W2 (default: true)\n";
	std::cout << "  --enable-r3         Enable R3 (inner loop, default: false)\n";
	std::cout
	    << "  --mode <n>    Mode: 0=all, 1=shared only, 2=local only (default: 0)\n\n";
	std::cout << "8 Orthogonal Characteristics (controlled by above options):\n";
	std::cout << "  1. Concurrency      -t (thread count)\n";
	std::cout << "  2. Transaction Length - --r1 and --w1 (total ops per tx)\n";
	std::cout << "  3. Working Set Size  - --a2 and --a3 (array sizes)\n";
	std::cout << "  4. Temporal Locality - --locality (data reuse patterns)\n";
	std::cout << "  5. Pollution        - Random vs sequential access pattern\n";
	std::cout << "  6. Contention       - --contention (shared vs private data)\n";
	std::cout << "  7. Predominance     - Ratio of --r1/--w1 to total ops\n";
	std::cout << "  8. Density          - --density (spatial locality)\n";
}

MAIN int main(int argc, char *argv[])
{
	g_config.threads = DEFAULT_NB_THREADS;
	g_config.duration = DEFAULT_DURATION_MS;
	g_config.R1 = 10;
	g_config.W1 = 10;
	g_config.R2 = 10;
	g_config.W2 = 10;
	g_config.A1 = 100;
	g_config.A2 = 10000;
	g_config.A3 = 10000;
	g_config.iterations = 0;
	g_config.contention = 0.5;
	g_config.locality = 0.5;
	g_config.density = 0.5;
	g_config.enable_R2 = true;
	g_config.enable_R3 = false;
	g_config.mode = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			g_config.threads = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			g_config.duration = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--r1") == 0 && i + 1 < argc) {
			g_config.R1 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--w1") == 0 && i + 1 < argc) {
			g_config.W1 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--r2") == 0 && i + 1 < argc) {
			g_config.R2 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--w2") == 0 && i + 1 < argc) {
			g_config.W2 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--a1") == 0 && i + 1 < argc) {
			g_config.A1 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--a2") == 0 && i + 1 < argc) {
			g_config.A2 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--a3") == 0 && i + 1 < argc) {
			g_config.A3 = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "--contention") == 0 && i + 1 < argc) {
			g_config.contention = std::atof(argv[++i]);
		} else if (strcmp(argv[i], "--locality") == 0 && i + 1 < argc) {
			g_config.locality = std::atof(argv[++i]);
		} else if (strcmp(argv[i], "--density") == 0 && i + 1 < argc) {
			g_config.density = std::atof(argv[++i]);
		} else if (strcmp(argv[i], "--enable-r2") == 0) {
			g_config.enable_R2 = true;
		} else if (strcmp(argv[i], "--disable-r2") == 0) {
			g_config.enable_R2 = false;
		} else if (strcmp(argv[i], "--enable-r3") == 0) {
			g_config.enable_R3 = true;
			g_config.iterations = 100;
		} else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
			g_config.mode = std::atoi(argv[++i]);
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	int loops = g_config.duration / 10;

	std::cout << "========= EigenBench =========\n";
	std::cout << "==============================\n";
	std::cout << "Configuration:\n";
	std::cout << "  Threads:        " << g_config.threads << "\n";
	std::cout << "  Duration:       " << g_config.duration << " ms\n";
	std::cout << "  R1/W1 (shared): " << g_config.R1 << "/" << g_config.W1
	          << " (contention=" << g_config.contention << ")\n";
	std::cout << "  R2/W2 (local):  " << g_config.R2 << "/" << g_config.W2
	          << " (locality=" << g_config.locality << ", density=" << g_config.density
	          << ")\n";
	std::cout << "  A1 (shared):    " << g_config.A1 << "\n";
	std::cout << "  A2 (local):     " << g_config.A2 << "\n";
	std::cout << "  A3 (non-tx):    " << g_config.A3 << "\n";
	std::cout << "  Mode:           " << g_config.mode << "\n";
	std::cout << "\n8 Characteristics:\n";
	std::cout << "  1. Concurrency:          " << g_config.threads << " threads\n";
	std::cout << "  2. Transaction Length:   " << (g_config.R1 + g_config.W1)
	          << " ops/tx\n";
	std::cout << "  3. Working Set Size:      A2=" << g_config.A2
	          << ", A3=" << g_config.A3 << "\n";
	std::cout << "  4. Temporal Locality:     " << g_config.locality << "\n";
	std::cout << "  5. Pollution:             "
	          << (g_config.locality < 0.5 ? "high (random)" : "low (sequential)") << "\n";
	std::cout << "  6. Contention:            " << g_config.contention << "\n";
	std::cout << "  7. Predominance:          R/W=" << g_config.R1 << "/" << g_config.W1
	          << " (" << (g_config.R1 * 100.0 / (g_config.R1 + g_config.W1))
	          << "% read)\n";
	std::cout << "  8. Density:               " << g_config.density << "\n";
	std::cout << std::endl;

	for (int i = 0; i < g_config.threads; i++) {
		g_array1[i] = 0;
	}
	for (int i = 0; i < g_config.threads * g_config.A2; i++) {
		g_array2[i] = i;
	}
	for (int i = 0; i < g_config.threads * g_config.A3; i++) {
		g_array3[i] = i;
	}

	std::vector<WorkerData> wd(g_config.threads);
	std::vector<std::thread> thr;
	std::vector<std::mt19937> rngs(g_config.threads);

	for (int i = 0; i < g_config.threads; i++) {
		rngs[i] = std::mt19937(i * 12345 + 42);
		wd[i].id = i;
		wd[i].loops = loops;
		wd[i].td.rng = &rngs[i];
		generate_access_patterns(wd[i].td, i);
	}

	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < g_config.threads; i++) {
		thr.emplace_back(worker, &wd[i]);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(g_config.duration));
	done = true;

	for (auto &t : thr)
		t.join();

	auto end = std::chrono::high_resolution_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

	uint64_t ops = total_ops.load();
	uint64_t reads = total_reads.load();
	uint64_t writes = total_writes.load();
	uint64_t tx = total_tx.load();

	std::cout << "\nResults\n";
	std::cout << "=======\n";
	std::cout << "Elapsed:     " << ms << " ms\n";
	std::cout << "Total ops:   " << ops << "\n";
	std::cout << "Ops/sec:     " << (ops * 1000.0 / ms) << "\n";
	std::cout << "Transactions: " << tx << "\n";
	std::cout << "Avg tx length: " << (tx > 0 ? (double)ops / tx : 0) << " ops/tx\n";
	std::cout << "Total reads:  " << reads << "\n";
	std::cout << "Total writes: " << writes << "\n";
	std::cout << "Read ratio:   "
	          << (reads + writes > 0 ? reads * 100.0 / (reads + writes) : 0) << "%\n";
	std::cout << std::endl;

	return 0;
}
