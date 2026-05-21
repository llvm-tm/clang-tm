#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM std::vector<int64_t> g_vec;

TX void fill_vec() {
  for (int i = 0; i < 5; i++) {
    g_vec.push_back(i + 1000);
  }
}

MAIN int main() {
  printf("Test: single TX, 5 push_back\n");
  fill_vec();
  size_t n = g_vec.size();
  printf("  g_vec.size() = %zu\n", n);
  bool ok = true;
  for (size_t i = 0; i < n; i++) {
    if (g_vec[i] != (int64_t)(i + 1000)) {
      printf("  FAIL at [%zu]: got %lld, expected %lld\n",
             i, (long long)g_vec[i], (long long)(i + 1000));
      ok = false;
    }
  }
  if (ok) printf("  PASS\n");
  return ok ? 0 : 1;
}
