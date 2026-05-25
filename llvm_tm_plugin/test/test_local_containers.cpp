#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

// NOTE: std::map uses _Rb_tree internally with opaque
// _Rb_tree_insert_and_rebalance (libstdc++ C function, body not visible in
// LLVM IR), so its internal pointer stores bypass TM write-set.  This test
// only exercises std::vector (which is fully inlineable).  For map-like
// operations inside TX, use TMSafeMap or TMTreapMap (see backends/).

TM std::atomic<int64_t> g_tx_count{0};

const int ITEMS_PER_TX = 50;
const int TXS_PER_THREAD = 100;
const int NUM_THREADS = 4;

TX void vector_tx(int thread_id, int base) {
  std::vector<int> local;
  local.reserve(ITEMS_PER_TX);
  for (int i = 0; i < ITEMS_PER_TX; i++) {
    local.push_back(base + i);
  }

  for (size_t i = 0; i < local.size(); i++) {
    if (local[i] != base + (int)i) {
      fprintf(stderr, "FAIL: vector_tx local verification failed at thread=%d base=%d i=%zu val=%d\n",
              thread_id, base, i, local[i]);
      fflush(stderr);
      std::exit(1);
    }
  }

  for (auto &v : local) {
    v = v * 3 + 1;
  }

  for (size_t i = 0; i < local.size(); i++) {
    if (local[i] != (base + (int)i) * 3 + 1) {
      fprintf(stderr, "FAIL: vector_tx local post-modify verification at thread=%d base=%d\n",
              thread_id, base);
      fflush(stderr);
      std::exit(1);
    }
  }

  g_tx_count.fetch_add(1);
}

THREAD void worker(int thread_id) {
  int base = thread_id * TXS_PER_THREAD * ITEMS_PER_TX;

  for (int i = 0; i < TXS_PER_THREAD; i++) {
    vector_tx(thread_id, base + i * ITEMS_PER_TX);
  }
}

MAIN int main() {
  printf("Local Containers Test (std::vector inside TX)\n");
  printf("=============================================\n\n");

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

  printf("\n====================================================\n");
  printf("PASS: All local containers tests passed\n");
  return 0;
}
