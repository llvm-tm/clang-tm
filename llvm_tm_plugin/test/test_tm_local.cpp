#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "tm_test_common.hpp"

// TM-shared global — should be instrumented
TM int64_t g_counter;

// TX function that uses a tm_local-annotated variable.
// The tm_local variable should NOT be instrumented (plain load/store),
// while the TM-shared global IS instrumented (tm_read/tm_write).
TX void inc_with_local(int64_t delta) {
  // This local variable is annotated tm_local: the plugin skips
  // instrumentation on its load/store.  The user asserts this
  // variable is thread-private.
  __attribute__((annotate("tm_local"))) int64_t tmp = 0;
  tmp = g_counter + delta;
  g_counter = tmp;
}

TX void multi_local() {
  __attribute__((annotate("tm_local"))) int64_t a = 0;
  __attribute__((annotate("tm_local"))) int64_t b = 0;
  a = g_counter + 1;
  b = a;
  g_counter = b;
}

MAIN int main(int argc, char* argv[]) {
  (void)argc; (void)argv;
  printf("test_tm_local: single-threaded verification\n");

  // Test 1: basic increment with tm_local
  g_counter = 0;
  inc_with_local(42);
  bool ok = (g_counter == 42);
  printf("  Test 1: g_counter = %lld (expected 42) %s\n",
         (long long)g_counter, ok ? "PASS" : "FAIL");

  // Test 2: multiple increments
  g_counter = 0;
  for (int i = 0; i < 100; i++)
    inc_with_local(1);
  bool ok2 = (g_counter == 100);
  printf("  Test 2: g_counter = %lld (expected 100) %s\n",
         (long long)g_counter, ok2 ? "PASS" : "FAIL");

  // Test 3: multiple tm_local variables
  g_counter = 0;
  multi_local();
  bool ok3 = (g_counter == 1);
  printf("  Test 3: multi_local g_counter = %lld (expected 1) %s\n",
         (long long)g_counter, ok3 ? "PASS" : "FAIL");

  bool all_ok = ok && ok2 && ok3;
  printf("  %s\n", all_ok ? "PASS" : "FAIL");
  return all_ok ? 0 : 1;
}
