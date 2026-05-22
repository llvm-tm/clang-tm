#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <random>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// Minimal vector-like struct with 3 pointers (same layout as libc++ vector)
struct MyVec {
  int64_t* begin_;
  int64_t* end_;
  int64_t* cap_;

  int64_t* data() const { return begin_; }
  size_t size() const { return size_t(end_ - begin_); }
  size_t capacity() const { return size_t(cap_ - begin_); }

  void grow(size_t min_cap) {
    size_t old_sz = size();
    size_t new_cap = old_sz * 2;
    if (new_cap < min_cap) new_cap = min_cap + 1;
    if (new_cap < 2) new_cap = 2;
    int64_t* buf = new int64_t[new_cap];
    for (size_t i = 0; i < old_sz; i++)
      buf[i] = begin_[i];
    delete[] begin_;
    begin_ = buf;
    end_ = buf + old_sz;
    cap_ = buf + new_cap;
  }
};

// Exact reproduction of libc++ _ConstructTransaction pattern
struct ConstructTransaction {
  MyVec& v_;
  int64_t* pos_;
  int64_t* const new_end_;

  ConstructTransaction(MyVec& v, size_t n)
    : v_(v), pos_(v.end_), new_end_(v.end_ + n) {}

  ~ConstructTransaction() {
    v_.end_ = pos_;
  }

  ConstructTransaction(const ConstructTransaction&) = delete;
  ConstructTransaction& operator=(const ConstructTransaction&) = delete;
};

TX void my_push_back(MyVec* v, int64_t val) {
  int64_t* end = v->end_;
  if (end < v->cap_) {
    // Non-reallocating path: exactly like libc++ __emplace_back_assume_capacity
    {
      ConstructTransaction tx(*v, 1);
      *tx.pos_ = val;
      ++tx.pos_;
    }
    ++end;
  } else {
    // Reallocating path
    v->grow(v->size() + 1);
    end = v->end_;
    {
      ConstructTransaction tx(*v, 1);
      *tx.pos_ = val;
      ++tx.pos_;
    }
    ++end;
  }
  v->end_ = end;
}

// ---------- Multi-threaded stress test ----------

TM MyVec g_vec{nullptr, nullptr, nullptr};

TM std::atomic<int64_t> g_pushes{0};
TM std::atomic<int64_t> g_sum{0};

std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

const int64_t ITEMS_PER_TX = 50;

TX void do_pushes(int64_t base) {
  for (int64_t i = 0; i < ITEMS_PER_TX; i++) {
    my_push_back(&g_vec, base + i);
  }
  g_pushes.fetch_add(ITEMS_PER_TX);
  g_sum.fetch_add(ITEMS_PER_TX * base + (ITEMS_PER_TX * (ITEMS_PER_TX - 1)) / 2);
}

THREAD void worker(int id) {
  std::mt19937 rng((unsigned)(id * 12345 + 1));
  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load()) {
    int64_t base = (int64_t)rng() % 1000000;
    do_pushes(base);
  }
}

MAIN int main(int argc, char* argv[]) {
  int duration = 3;
  int n_threads = 2;
  if (argc > 1) duration = atoi(argv[1]);
  if (argc > 2) n_threads = atoi(argv[2]);

  printf("ConstructTransaction Pattern Test\n");
  printf("  duration: %ds  threads: %d\n\n", duration, n_threads);

  std::thread* threads = new std::thread[n_threads];
  for (int i = 0; i < n_threads; i++)
    threads[i] = std::thread(worker, i);

  g_start.store(true);
  std::this_thread::sleep_for(std::chrono::seconds(duration));
  g_stop.store(true);

  for (int i = 0; i < n_threads; i++)
    threads[i].join();

  size_t sz = g_vec.size();
  int64_t pushes = g_pushes.load();
  int64_t sum = g_sum.load();

  printf("Results:\n");
  printf("  g_vec.size() = %zu\n", sz);
  printf("  g_vec pushes = %lld\n", (long long)pushes);
  printf("  g_vec sum    = %lld\n", (long long)sum);

  bool ok = true;
  if (sz > (size_t)pushes) {
    printf("  FAIL: size > pushes (elements lost)\n");
    ok = false;
  }

  // Verify contents (spot check)
  int64_t check_sum = 0;
  for (size_t i = 0; i < sz; i++) {
    check_sum += g_vec.data()[i];
  }

  if (check_sum != sum) {
    printf("  FAIL: sum mismatch  got=%lld  expected=%lld\n",
           (long long)check_sum, (long long)sum);
    // Check for NULL end_ / corrupted state
    printf("  g_vec.begin_=%p  g_vec.end_=%p  g_vec.cap_=%p\n",
           (void*)g_vec.begin_, (void*)g_vec.end_, (void*)g_vec.cap_);
    ok = false;
  }

  delete[] threads;
  printf("  %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
