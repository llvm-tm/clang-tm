#include <cstdio>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM int64_t g_val;

TX void do_write() {
  g_val = 42;
}

MAIN int main() {
  printf("Start test\n");
  do_write();
  int64_t v = g_val;
  printf("  g_val = %lld (expected 42)\n", (long long)v);
  bool ok = (v == 42);
  printf("  %s\n", ok ? "PASS" : "FAIL");
  fflush(stdout);
  return ok ? 0 : 1;
}
