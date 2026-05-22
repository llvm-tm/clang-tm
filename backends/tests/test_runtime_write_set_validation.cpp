/**
 * Write-Set Validation Test
 *
 * Tests the invariant: every transaction that writes to an address must
 * have that address validated during commit.  Without write-set-to-read-set
 * propagation in write_word_ctl (path 5), a transaction can commit after
 * another transaction has already written to the same address, producing a
 * lost update on that address.
 *
 * Design:
 *   Two shared variables A, B (different 8-byte-aligned addresses, different locks).
 *   TX: read A -> write A+1 -> write B = A*COEFF (B is write-set-only — never pre-read)
 *
 *   Without write-set validation: B is never in the read-set, so validate()
 *   ignores it.  The final B reflects a stale A snapshot, producing an
 *   inconsistent (A, B) pair.
 *
 *   With write-set validation: B is added to read_set at write time, so any
 *   version change on B's lock during commit triggers an abort and retry.
 */

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();
void tm_begin();
void tm_end();
uint64_t tm_read_i8(volatile uint64_t *addr);
void tm_write_i8(volatile uint64_t *addr, uint64_t val);
}

struct Data {
  volatile uint64_t a;  // offset 0 -> different lock from b
  volatile uint64_t b;  // offset 8
};

Data data{0, 0};

constexpr int NUM_THREADS = 4;
constexpr int ITERS_PER_THREAD = 5000;
constexpr uint64_t COEFF = 10;

int main() {
  printf("Write-Set Validation Test\n");
  printf("=========================\n\n");
  printf("Threads:    %d\n", NUM_THREADS);
  printf("Iterations: %d per thread\n", ITERS_PER_THREAD);
  printf("Expected:   a=%d, b=(a-1)*%llu\n\n",
         NUM_THREADS * ITERS_PER_THREAD, (unsigned long long)COEFF);

  tm_init();

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([i]() {
      tm_init_thread();
      tm_nested_call_counter++;

      for (int j = 0; j < ITERS_PER_THREAD; ++j) {
        int committed = 0;
        while (!committed) {
          tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
          tm_begin();

          if (tm_longjmp_ret != 0)
            continue;

          uint64_t a_val = tm_read_i8(&data.a);
          tm_write_i8(&data.a, a_val + 1);
          // write to B without pre-reading — write-set-only address
          tm_write_i8(&data.b, a_val * COEFF);

          tm_end();
          committed = 1;
        }
      }

      tm_exit_thread();
    });
  }

  for (auto &t : threads)
    t.join();

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  uint64_t final_a = data.a;
  uint64_t final_b = data.b;
  uint64_t expected_b = (final_a > 0) ? (final_a - 1) * COEFF : 0;

  printf("Results:\n");
  printf("  a = %llu (expected %u)\n",
         (unsigned long long)final_a, NUM_THREADS * ITERS_PER_THREAD);
  printf("  b = %llu (expected %llu)\n",
         (unsigned long long)final_b, (unsigned long long)expected_b);
  printf("  Time: %llu ms\n", (unsigned long long)ms.count());

  bool ok = true;

  if (final_a != (uint64_t)(NUM_THREADS * ITERS_PER_THREAD)) {
    printf("  FAIL: a=%llu != expected=%u\n",
           (unsigned long long)final_a, NUM_THREADS * ITERS_PER_THREAD);
    ok = false;
  }

  if (final_b != expected_b) {
    printf("  FAIL: b=%llu != expected=%llu — lost update on B\n",
           (unsigned long long)final_b, (unsigned long long)expected_b);
    ok = false;
  }

  printf("\n  Result: %s\n", ok ? "PASS" : "FAIL");
  tm_exit();
  return ok ? 0 : 1;
}
