#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

// Shared TM vector — starts with 0 capacity so every push triggers reallocation
TM std::vector<int64_t> g_vec;

const int PUSHES_PER_TX = 100;
const int TXS_PER_THREAD = 4;
const int NUM_READ_TXS = 40;

std::atomic<bool> g_start{false};
std::atomic<bool> g_done{false};

TX void push_elements(int base) {
  for (int i = 0; i < PUSHES_PER_TX; i++) {
    g_vec.push_back(base + i);
  }
}

TX void read_all() {
  volatile int64_t sum = 0;
  for (size_t i = 0; i < g_vec.size(); i++) {
    sum += g_vec[i];
  }
}

THREAD void writer() {
  while (!g_start.load()) std::this_thread::yield();
  for (int t = 0; t < TXS_PER_THREAD; t++) {
    push_elements(t * PUSHES_PER_TX);
  }
  g_done.store(true);
}

THREAD void concurrent_reader() {
  while (!g_start.load()) std::this_thread::yield();
  for (int i = 0; i < NUM_READ_TXS; i++) {
    read_all();
  }
}

MAIN int main() {
  printf("Vector Concurrent Reallocation Test\n");
  printf("====================================\n\n");

  std::thread w(writer);
  std::thread r(concurrent_reader);

  g_start.store(true);

  w.join();
  r.join();

  size_t final_size = g_vec.size();
  size_t expected = TXS_PER_THREAD * PUSHES_PER_TX;
  printf("  g_vec.size() = %zu (expected %zu)\n", final_size, expected);

  bool ok = true;
  for (size_t i = 0; i < final_size; i++) {
    int64_t expected_val = (i / PUSHES_PER_TX) * PUSHES_PER_TX + (i % PUSHES_PER_TX);
    int64_t val = g_vec[i];
    if (val != expected_val) {
      printf("  FAIL: g_vec[%zu] = %lld, expected %lld\n", i, (long long)val, (long long)expected_val);
      ok = false;
    }
  }

  if (ok) {
    printf("\n  Result: PASS\n");
    return 0;
  }
  printf("\n  Result: FAIL\n");
  return 1;
}
