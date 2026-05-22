#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

struct Node {
  int64_t key;
  Node *next;
};

TM Node *g_head = nullptr;
TM int64_t g_count{0};

std::atomic<bool> g_start{false};
std::atomic<bool> g_stop{false};

TX void insert(int64_t key) {
  Node *n = new Node{key, nullptr};

  if (g_head == nullptr || key < g_head->key) {
    n->next = g_head;
    g_head = n;
    return;
  }

  Node *cur = g_head;
  while (cur->next && cur->next->key < key)
    cur = cur->next;

  n->next = cur->next;
  cur->next = n;
}

TX void remove(int64_t key) {
  if (!g_head) return;

  if (g_head->key == key) {
    Node *old = g_head;
    g_head = old->next;
    delete old;
    return;
  }

  Node *cur = g_head;
  while (cur->next) {
    if (cur->next->key == key) {
      Node *old = cur->next;
      cur->next = old->next;
      delete old;
      return;
    }
    cur = cur->next;
  }
}

TX void verify() {
  Node *cur = g_head;
  while (cur) {
    if (cur->next && cur->next->key <= cur->key)
      g_count++;
    cur = cur->next;
  }
}

THREAD void inserter(int id) {
  std::mt19937 rng((unsigned)(id * 13579 + 1));
  std::uniform_int_distribution<int64_t> dist(0, 1999);
  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load())
    insert(dist(rng));
}

THREAD void remover(int id) {
  std::mt19937 rng((unsigned)(id * 13579 + 2));
  std::uniform_int_distribution<int64_t> dist(0, 1999);
  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load())
    remove(dist(rng));
}

THREAD void drain() {
  Node *cur = g_head;
  while (cur) {
    Node *old = cur;
    cur = cur->next;
    delete old;
  }
  g_head = nullptr;
}

THREAD void verifier() {
  while (!g_start.load()) std::this_thread::yield();
  while (!g_stop.load())
    verify();
}

int main() {
  int duration = 3;
  int n_ins = 2;
  int n_rem = 2;

  printf("Linked-List Test (no atomic counters)\n");
  printf("=====================================\n");

  std::vector<std::thread> threads;
  for (int i = 0; i < n_ins; i++) threads.emplace_back(inserter, i);
  for (int i = 0; i < n_rem; i++) threads.emplace_back(remover, i);
  threads.emplace_back(verifier);

  g_start.store(true);
  std::this_thread::sleep_for(std::chrono::seconds(duration));
  g_stop.store(true);

  for (auto &t : threads) t.join();

  printf("\n  Final list: ");
  Node *cur = g_head;
  int64_t n = 0, errs = 0;
  while (cur) {
    if (cur->next && cur->next->key <= cur->key) {
      printf("(%lld->%lld) ", (long long)cur->key, (long long)cur->next->key);
      errs++;
    }
    n++;
    cur = cur->next;
  }
  printf("\n  size = %lld, ordering violations = %lld\n",
         (long long)n, (long long)errs);

  if (errs) {
    printf("  FAIL: ordering violations in final list\n");
    return 1;
  }
  printf("  PASS\n");
  return 0;
}
