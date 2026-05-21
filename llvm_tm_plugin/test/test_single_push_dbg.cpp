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

// Get pointer directly (without TM read)
int64_t* get_begin() { return &*g_vec.begin(); }
int64_t* get_end()   { return g_vec.data() + g_vec.size(); }
size_t   get_size()  { return g_vec.size(); }

MAIN int main() {
  printf("Start\n");
  fflush(stdout);
  
  do_push();
  
  printf("  g_vec size = %zu\n", g_vec.size());
  printf("  g_vec[0] = %lld\n", (long long)g_vec[0]);
  printf("  g_vec.data() = %p\n", (void*)g_vec.data());
  printf("  g_vec.begin() = %p\n", (void*)&*g_vec.begin());
  printf("  g_vec.end() = %p\n", (void*)&*g_vec.end());
  
  // Check if pointers look sane
  int64_t* b = g_vec.data();
  int64_t* e = b + g_vec.size();
  size_t cap = g_vec.capacity();
  printf("  begin=%p end=%p cap=%zu data=%p\n", (void*)b, (void*)e, cap, (void*)g_vec.data());
  fflush(stdout);
  
  bool ok = (g_vec.size() == 1 && g_vec[0] == 42);
  printf("  %s\n", ok ? "PASS" : "FAIL");
  fflush(stdout);
  return ok ? 0 : 1;
}
