#include "tm_safe_map.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

// Multiple TM globals to stress spec alloc + deferred free

// 1. std::vector + push_back => new[] / delete[] on reallocation
TM std::vector<int64_t> g_vec;
TM std::atomic<int64_t> g_vec_total{0};
TM std::atomic<int64_t> g_vec_pushes{0};

// 2. std::map + insert/erase => new/delete of tree nodes
TM TMSafeMap<int64_t, int64_t> g_map;
TM std::atomic<int64_t> g_map_ops{0};

// 3. raw new/delete inside TX
TM int64_t *g_raw_ptr = nullptr;
TM std::atomic<int64_t> g_raw_written{0};

// Sync
std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

const int64_t ITEMS_PER_VEC_TX = 200;
const int64_t MAP_INSERTS_PER_TX = 32;
const int64_t MAP_ERASES_PER_TX = 16;
const int64_t RAW_NEWS_PER_TX = 8;

// ---- TX functions that allocate and free ----

TX void vec_push_tx(int64_t base)
{
	int64_t count = 0;
	for (int64_t i = 0; i < ITEMS_PER_VEC_TX; i++) {
		g_vec.push_back(base + i);
		count++;
	}
	g_vec_pushes.fetch_add(count);
	g_vec_total.fetch_add(count * base + (count * (count - 1)) / 2);
}

TX void map_insert_tx(int64_t base)
{
	for (int64_t i = 0; i < MAP_INSERTS_PER_TX; i++) {
		g_map[base + i] = (base + i) * 10;
	}
	printf("Passou aqui!\n");
	g_map_ops.fetch_add(MAP_INSERTS_PER_TX);
}

TX void map_erase_tx(int64_t base)
{
	for (int64_t i = 0; i < MAP_ERASES_PER_TX; i++) {
		g_map.erase(base + i);
	}
	g_map_ops.fetch_add(MAP_ERASES_PER_TX);
}

TX void raw_new_delete_tx(int64_t val)
{
	for (int64_t i = 0; i < RAW_NEWS_PER_TX; i++) {
		int64_t *p = new int64_t(val + i);
		*p = *p + 1;
		g_raw_written.fetch_add(*p);
		delete p;
	}
}

TX void vec_and_map_tx(int64_t base)
{
	// Mix both in a single TX to interleave spec allocs and deferred frees
	for (int64_t i = 0; i < 10; i++) {
		g_vec.push_back(base + i);
	}
	for (int64_t i = 0; i < 10; i++) {
		g_map[base + i] = (base + i) * 10;
	}
	g_vec_pushes.fetch_add(10);
	g_map_ops.fetch_add(10);
}

// ---- Workers ----

THREAD void vec_worker(int id)
{
	std::mt19937 rng((unsigned)(id * 12345 + 1));
	while (!g_start.load())
		std::this_thread::yield();
	while (!g_stop.load()) {
		int64_t base = (int64_t)rng() % 1000000;
		vec_push_tx(base);
	}
}

THREAD void map_insert_worker(int id)
{
	std::mt19937 rng((unsigned)(id * 12345 + 2));
	while (!g_start.load())
		std::this_thread::yield();
	while (!g_stop.load()) {
		int64_t base = (int64_t)rng() % 1000000;
		map_insert_tx(base);
	}
}

THREAD void map_erase_worker(int id)
{
	std::mt19937 rng((unsigned)(id * 12345 + 3));
	while (!g_start.load())
		std::this_thread::yield();
	while (!g_stop.load()) {
		int64_t base = (int64_t)rng() % 1000000;
		map_erase_tx(base);
	}
}

THREAD void raw_new_delete_worker(int id)
{
	std::mt19937 rng((unsigned)(id * 12345 + 4));
	while (!g_start.load())
		std::this_thread::yield();
	while (!g_stop.load()) {
		int64_t val = (int64_t)rng() % 1000000;
		raw_new_delete_tx(val);
	}
}

THREAD void mixed_worker(int id)
{
	std::mt19937 rng((unsigned)(id * 12345 + 5));
	while (!g_start.load())
		std::this_thread::yield();
	while (!g_stop.load()) {
		int64_t base = (int64_t)rng() % 1000000;
		vec_and_map_tx(base);
	}
}

// ---- Main ----

MAIN int main(int argc, char *argv[])
{
	int duration = 3;
	int n_vec = 2;
	int n_map_ins = 2;
	int n_map_ers = 1;
	int n_raw = 1;
	int n_mixed = 1;

	for (int i = 1; i < argc; i++) {
		if (i + 1 < argc) {
			if (strcmp(argv[i], "-d") == 0)
				duration = atoi(argv[++i]);
			else if (strcmp(argv[i], "-v") == 0)
				n_vec = atoi(argv[++i]);
			else if (strcmp(argv[i], "-i") == 0)
				n_map_ins = atoi(argv[++i]);
			else if (strcmp(argv[i], "-e") == 0)
				n_map_ers = atoi(argv[++i]);
			else if (strcmp(argv[i], "-r") == 0)
				n_raw = atoi(argv[++i]);
			else if (strcmp(argv[i], "-m") == 0)
				n_mixed = atoi(argv[++i]);
		}
	}

	printf("Alloc Stress Test\n");
	printf("=================\n\n");
	printf("Duration: %ds\n", duration);
	printf("  vec workers:       %d\n", n_vec);
	printf("  map insert workers: %d\n", n_map_ins);
	printf("  map erase workers:  %d\n", n_map_ers);
	printf("  raw new/delete:     %d\n", n_raw);
	printf("  mixed workers:      %d\n\n", n_mixed);

	int total_threads = n_vec + n_map_ins + n_map_ers + n_raw + n_mixed;
	std::vector<std::thread> threads;

	for (int i = 0; i < n_vec; i++)
		threads.emplace_back(vec_worker, i);
	for (int i = 0; i < n_map_ins; i++)
		threads.emplace_back(map_insert_worker, i);
	for (int i = 0; i < n_map_ers; i++)
		threads.emplace_back(map_erase_worker, i);
	for (int i = 0; i < n_raw; i++)
		threads.emplace_back(raw_new_delete_worker, i);
	for (int i = 0; i < n_mixed; i++)
		threads.emplace_back(mixed_worker, i);

	g_start.store(true);
	std::this_thread::sleep_for(std::chrono::seconds(duration));
	g_stop.store(true);

	for (auto &t : threads)
		t.join();

	printf("\nResults:\n");
	printf("  g_vec.size() = %zu\n", g_vec.size());
	printf("  g_vec pushes = %lld\n", (long long)g_vec_pushes.load());
	printf("  g_map.size() = %zu\n", g_map.size());
	printf("  g_map ops    = %lld\n", (long long)g_map_ops.load());
	printf("  raw written  = %lld\n", (long long)g_raw_written.load());

	bool ok = true;
	if (g_vec.size() > g_vec_pushes.load()) {
		printf("  FAIL: vec size > pushes\n");
		ok = false;
	}

	// Verify map keys are in valid range
	for (auto &kv : g_map) {
		if (kv.second != kv.first * 10) {
			printf("  FAIL: g_map[%lld] = %lld (expected %lld)\n",
			       (long long)kv.first,
			       (long long)kv.second,
			       (long long)(kv.first * 10));
			ok = false;
			break;
		}
	}

	if (ok) {
		printf("\n  Result: PASS (no corruption detected)\n");
		return 0;
	}
	printf("\n  Result: FAIL\n");
	return 1;
}
