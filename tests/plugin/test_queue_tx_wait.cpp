/// Test: tm_enqueue_ex + tm_wait_tx
///
/// Enqueue 5 TXes on queue 0, each incrementing a shared counter.
/// Wait for TX #3 (non-last), verify partial progress (at least 3 done),
/// then wait-all for remaining, verify all 5 complete.

#include "queue_runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct AddArgs {
	std::atomic<int> *counter;
	int delta;
};

static void add_one(void *args)
{
	auto *a = static_cast<AddArgs *>(args);
	a->counter->fetch_add(a->delta, std::memory_order_relaxed);
	delete a;
}

int main()
{
	tm_queue_init(4, 1);

	std::atomic<int> counter{0};

	uint64_t ids[5];
	for (int i = 0; i < 5; i++) {
		auto *args = new AddArgs{&counter, i + 1};
		ids[i] = tm_enqueue_ex(add_one, args, 0);
	}

	// Wait for TX #3 (index 2, 0-based) — not the last one enqueued.
	tm_wait_tx(ids[2]);

	// At this point TX #3 must be done, but 4 and 5 may still be pending.
	int val = counter.load(std::memory_order_acquire);
	if (val < 6) {
		printf("FAIL: after waiting for TX#3, counter=%d (expected >= 6)\n", val);
		return 1;
	}
	printf("partially done: counter=%d\n", val);

	// Now wait for all remaining
	tm_wait_prev_tx();

	val = counter.load(std::memory_order_acquire);
	if (val != 15) {
		printf("FAIL: after wait-all, counter=%d (expected 15)\n", val);
		return 1;
	}

	printf("PASS\n");
	tm_queue_shutdown();
	return 0;
}
