#include <cstdio>
#include <cstdint>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM std::vector<int64_t> g_vec;

TX void do_push() {
  g_vec.push_back(42);
}

MAIN int main() {
  printf("Single push_back in TX\n");
  do_push();
  size_t n = g_vec.size();
  printf("  g_vec.size() = %zu (expected 1)\n", n);
  if (n > 0) {
    printf("  g_vec[0] = %lld (expected 42)\n", (long long)g_vec[0]);
  }
  bool ok = (n == 1 && g_vec[0] == 42);
  printf("  %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
