/// Test: multi-threaded queue-based transaction execution
/// Multiple threads call TX functions which are enqueued to the
/// thread pool for execution.  Verifies write-back from pool
/// workers is visible to all threads after each sync TX completes.
///
/// KNOWN RACE: `counter` is a plain `static int`, not TM-tracked.
/// The TX annotation wraps the body in tm_begin/tm_end, but the LLVM
/// pass only instrument loads/stores to TM-annotated globals. Four
/// threads doing `counter += 1` without atomic/lock protection causes
/// lost updates. Expected 400, observed ~351.
///
/// Fix: either declare counter as `TM int counter` (TM-tracked global)
/// or use `std::atomic<int>`. Also, caller threads should call
/// `tm_wait_prev_tx()` after enqueuing to ensure pool workers finish
/// before main checks the result.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <pthread.h>
#include <vector>

#include "tm_test_common.hpp"

static int counter = 0;
static const int num_threads = 4;
static const int iters_per_thread = 100;

TX void tx_increment(int delta) {
    counter += delta;
}

THREAD void worker(int tid) {
    for (int i = 0; i < iters_per_thread; i++) {
        tx_increment(1);
    }
}

static void* thread_entry(void* arg) {
    int tid = (int)(intptr_t)arg;
    worker(tid);
    return nullptr;
}

MAIN int main() {
    counter = 0;
    std::vector<pthread_t> threads(num_threads);
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], nullptr, thread_entry, (void*)(intptr_t)i);
    }
    for (auto &t : threads) {
        pthread_join(t, nullptr);
    }
    int expected = num_threads * iters_per_thread;
    if (counter == expected) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL: counter=%d expected=%d\n", counter, expected);
        return 1;
    }
}
