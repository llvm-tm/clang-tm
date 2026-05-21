#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// Sorted linked-list node — heap-allocated via new/delete inside TX
struct Node {
  int64_t key;
  int64_t val;
  Node *next;
};

TM Node *g_head = nullptr;

TM std::atomic<int64_t> g_inserted{0};
TM std::atomic<int64_t> g_removed{0};
TM std::atomic<int64_t> g_errors{0};

std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

// ── Sorted insert ──────────────────────────────────────────────────
// Allocates a Node (speculative malloc).  On abort the spec alloc is
// freed; on commit the node stays and the list pointer is visible.
TX void insert(int64_t key, int64_t val) {
  Node *n = new Node{key, val, nullptr};

  if (g_head == nullptr || key < g_head->key) {
    n->next = g_head;
    g_head = n;
    g_inserted.fetch_add(1);
    return;
  }

  Node *cur = g_head;
  while (cur->next && cur->next->key < key)
    cur = cur->next;

  n->next = cur->next;
  cur->next = n;
  g_inserted.fetch_add(1);
}

// ── Remove by key ──────────────────────────────────────────────────
// Unlinks the node and deletes it (deferred free).  On commit the
// deferred free runs; on abort the node stays alive (TM rollback
// restores the list pointer).
TX void remove(int64_t key) {
  if (!g_head) return;

  if (g_head->key == key) {
    Node *old = g_head;
    g_head = old->next;
    delete old;
    g_removed.fetch_add(1);
    return;
  }

  Node *cur = g_head;
  while (cur->next) {
    if (cur->next->key == key) {
      Node *old = cur->next;
      cur->next = old->next;
      delete old;
      g_removed.fetch_add(1);
      return;
    }
    cur = cur->next;
  }
}

// ── Traverse + verify sorted order ─────────────────────────────────
TX void verify() {
  Node *cur = g_head;
  while (cur) {
    if (cur->next && cur->next->key < cur->key)
      g_errors.fetch_add(1);
    cur = cur->next;
  }
}

// ── Drain all nodes ────────────────────────────────────────────────
TX void drain() {
  while (g_head) {
    Node *old = g_head;
    g_head = old->next;
    delete old;
  }
}

// ── Workers ────────────────────────────────────────────────────────

THREAD void inserter(int id) {
  std::mt19937 rng((unsigned)(id * 13579 + 1));
  std::uniform_int_distribution<int64_t> dist(0, 1999);

  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load())
    insert(dist(rng), dist(rng) * 10);
}

THREAD void remover(int id) {
  std::mt19937 rng((unsigned)(id * 13579 + 2));
  std::uniform_int_distribution<int64_t> dist(0, 1999);

  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load())
    remove(dist(rng));
}

THREAD void verifier(int id) {
  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load())
    verify();
}

// ── Main ───────────────────────────────────────────────────────────

MAIN int main(int argc, char *argv[]) {
  int duration  = 3;
  int n_ins     = 2;
  int n_rem     = 2;
  int n_ver     = 1;

  for (int i = 1; i < argc; i++) {
    if (i + 1 < argc) {
      if (strcmp(argv[i], "-d") == 0)   duration = atoi(argv[++i]);
      if (strcmp(argv[i], "-ins") == 0) n_ins    = atoi(argv[++i]);
      if (strcmp(argv[i], "-rem") == 0) n_rem    = atoi(argv[++i]);
      if (strcmp(argv[i], "-ver") == 0) n_ver    = atoi(argv[++i]);
    }
  }

  printf("Linked-List Alloc Stress Test\n");
  printf("=============================\n");
  printf("Duration: %ds  Threads: %d\n", duration, n_ins + n_rem + n_ver);
  printf("  inserters: %d\n", n_ins);
  printf("  removers:  %d\n", n_rem);
  printf("  verifiers: %d\n\n", n_ver);

  std::vector<std::thread> threads;
  for (int i = 0; i < n_ins; i++) threads.emplace_back(inserter, i);
  for (int i = 0; i < n_rem; i++) threads.emplace_back(remover, i);
  for (int i = 0; i < n_ver; i++) threads.emplace_back(verifier, i);

  g_start.store(true);
  std::this_thread::sleep_for(std::chrono::seconds(duration));
  g_stop.store(true);

  for (auto &t : threads) t.join();

  drain();

  int64_t ins = g_inserted.load();
  int64_t rem = g_removed.load();
  int64_t err = g_errors.load();
  printf("\nResults:\n");
  printf("  inserted: %lld\n", (long long)ins);
  printf("  removed:  %lld\n", (long long)rem);
  printf("  errors:   %lld\n", (long long)err);
  printf("\n");

  if (g_head) {
    printf("  FAIL: list not empty after drain\n");
    return 1;
  }
  if (err) {
    printf("  FAIL: %lld ordering violations\n", (long long)err);
    return 1;
  }
  printf("  Result: PASS\n");
  return 0;
}
