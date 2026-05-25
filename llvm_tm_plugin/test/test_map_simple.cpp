#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

TM std::atomic<int64_t> g_tx_count{0};

const int ITEMS_PER_TX = 10;
const int TXS_PER_THREAD = 5;
const int NUM_THREADS = 4;

TX void map_tx(int thread_id, int base) {
  std::map<int, int> local;
  for (int i = 0; i < ITEMS_PER_TX; i++) {
    local[base + i] = (base + i) * 10;
  }

  for (int i = 0; i < ITEMS_PER_TX; i++) {
    if (local[base + i] != (base + i) * 10) {
      fprintf(stderr, "FAIL: map_tx insert verification at thread=%d base=%d i=%d\n",
              thread_id, base, i);
      fflush(stderr);
      std::exit(1);
    }
  }

  g_tx_count.fetch_add(1);
}

THREAD void worker(int thread_id) {
  int base = thread_id * TXS_PER_THREAD * ITEMS_PER_TX;

  for (int i = 0; i < TXS_PER_THREAD; i++) {
    map_tx(thread_id, base + i * ITEMS_PER_TX);
  }
}

MAIN int main() {
  printf("Map Simple Test\n");
  printf("==============\n\n");

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; i++) {
    threads.emplace_back(worker, i);
  }
  for (auto &t : threads) {
    t.join();
  }

  int64_t tx_count = g_tx_count.load();
  int64_t expected_tx_count = NUM_THREADS * TXS_PER_THREAD;
  if (tx_count != expected_tx_count) {
    fprintf(stderr, "FAIL: g_tx_count = %lld, expected %lld\n",
            (long long)tx_count, (long long)expected_tx_count);
    return 1;
  }
  printf("  g_tx_count = %lld (expected %lld)  PASS\n",
         (long long)tx_count, (long long)expected_tx_count);

  printf("\n==============\n");
  printf("PASS: Map simple test passed\n");
  return 0;
}
