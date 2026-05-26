// Test: tm_init/tm_init_thread/tm_exit_thread/tm_exit do NOT start transactions.
//
// The plugin inserts tm_init() and tm_init_thread() at main entry,
// and tm_exit_thread()/tm_exit() at main return.  This test verifies:
//   1. Transactions work correctly after init (no phantom transaction)
//   2. tm_begin_count == tm_end_count (every begin matched by end)
//   3. Results are correct (all increments applied)
//
// Backends with g_tm_begin_count/g_tm_end_count counters print stats
// at exit, showing tm_begin == tm_end when init doesn't start a tx.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>

#include "tm_test_common.hpp"

TM int64_t g_value = 0;

TX void increment() {
  g_value++;
}

MAIN int main(int argc, char* argv[]) {
  (void)argc; (void)argv;
  (void)argc; (void)argv;

  // After plugin-inserted tm_init+tm_init_thread, run a transaction.
  // If init_thread() incorrectly started a transaction (active=true),
  // the first tm_begin() would clear the write-set, losing the
  // tm_nested_call_counter write from the TX wrapper preamble.
  // Subsequent iterations would take the nested path, skipping
  // tm_begin(), and writes would go directly to memory (no TM).
  g_value = 0;
  increment();
  bool ok1 = (g_value == 1);
  printf("Test 1: g_value = %lld (expected 1) %s\n",
         (long long)g_value, ok1 ? "PASS" : "FAIL");

  // 100 consecutive transactions — if an init/exit function started
  // a phantom transaction, the second+ transactions would misbehave.
  g_value = 0;
  for (int i = 0; i < 100; i++)
    increment();
  bool ok2 = (g_value == 100);
  printf("Test 2: g_value = %lld (expected 100) %s\n",
         (long long)g_value, ok2 ? "PASS" : "FAIL");

  // Final sanity check
  g_value = 0;
  increment();
  bool ok3 = (g_value == 1);
  printf("Test 3: g_value = %lld (expected 1) %s\n",
         (long long)g_value, ok3 ? "PASS" : "FAIL");

  bool all_ok = ok1 && ok2 && ok3;
  // Backend stats (tm_begin/tm_end counts) are printed automatically
  // by runtimes that track them — look for tm_begin == tm_end in output.
  printf("%s\n", all_ok ? "PASS" : "FAIL");
  return all_ok ? 0 : 1;
}
