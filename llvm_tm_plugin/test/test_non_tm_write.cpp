#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

struct Node {
  int64_t key;
  int64_t val;
  Node *next;
};

TM Node *g_head = nullptr;
TM std::atomic<int64_t> g_seen{0};
TM std::atomic<int64_t> g_aborts{0};

TX void reader_tx() {
  Node *cur = g_head;
  while (cur) {
    g_seen.fetch_add(cur->val);
    cur = cur->next;
  }
}

THREAD void reader_thread(int id) {
  // back-to-back TXs — forces repeated reads that may reuse read_set entries
  for (int i = 0; i < 50000; i++)
    reader_tx();
}

THREAD void writer_thread_non_tm() {
  // Directly modify memory (bypassing TM) to simulate a missing store
  g_head = nullptr;
}

MAIN int main() {
  // Build a small list via TM
  Node *n2 = new Node{2, 20, nullptr};
  Node *n1 = new Node{1, 10, n2};
  g_head = n1;

  printf("Non-TM write test\n");
  printf("=================\n");
  printf("g_head initially = %p (key=1)\n", (void*)g_head);

  std::thread rdr(reader_thread, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::thread wtr(writer_thread_non_tm);

  rdr.join();
  wtr.join();

  int64_t seen = g_seen.load();
  int64_t orig = g_head == nullptr ? 0 : g_head->key;
  printf("  g_seen  = %lld\n", (long long)seen);
  printf("  g_head  = %p (key=%lld)\n", (void*)g_head, (long long)orig);
  printf("  DONE\n");
  return 0;
}
