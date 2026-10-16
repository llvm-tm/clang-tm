/// Benchmark: compare auto-wait (per-TX tm_wait_prev_tx) vs manual-wait
/// (batch enqueue + single tm_wait_prev_tx at end).
///
/// Auto-wait: TX calls are synchronous — each TX completes before the next
/// starts, because tm_wait_prev_tx() is injected after every enqueue.
///
/// Manual-wait: all TXes are enqueued first, then a single wait drains
/// the batch. Workers can execute multiple TXes in parallel.
///
/// Build variants:
///   tm-instrument-queue:        uses "shared" → auto-wait
///   tm-instrument-queue-manual: uses "async_shared" + explicit wait

#include <chrono>
#include <cstdio>

#ifndef MANUAL
// ---- Auto-wait variant ----
// Plugin injects tm_enqueue + tm_wait_prev_tx per call.
__attribute__((noinline, annotate("shared"))) void tx_work(int *p, int iters)
{
	for (int j = 0; j < iters; j++)
		(*p)++;
}
#else
// ---- Manual-wait variant ----
// Plugin injects only tm_enqueue; application calls tm_wait_prev_tx.
extern "C" void (*tm_wait_prev_tx)(void);

__attribute__((noinline, annotate("async_shared"))) void tx_work(int *p, int iters)
{
	for (int j = 0; j < iters; j++)
		(*p)++;
}
#endif

int main()
{
	// Workload: N_TX transactions, each doing ITERS increments.
	// Total operations = N_TX * ITERS.
	// Use separate counters so TXes don't contend.
	const int N_TX = 2000;
	const int ITERS = 5000;

	static int data[N_TX];

	// --- Warmup ---
	for (int t = 0; t < 5; t++) {
		for (int i = 0; i < N_TX; i++)
			data[i] = 0;
		for (int i = 0; i < N_TX; i++)
			tx_work(&data[i], ITERS);
#ifndef MANUAL
		// auto-wait: each call already waited — no extra sync needed
#else
		tm_wait_prev_tx();
#endif
	}

	// --- Timed run ---
	for (int i = 0; i < N_TX; i++)
		data[i] = 0;
	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < N_TX; i++)
		tx_work(&data[i], ITERS);
#ifndef MANUAL
	// (no extra sync — auto-wait ensured each completed)
#else
	tm_wait_prev_tx();
#endif
	auto end = std::chrono::high_resolution_clock::now();
	double sec = std::chrono::duration<double>(end - start).count();

	// --- Verify ---
	int errors = 0;
	for (int i = 0; i < N_TX; i++)
		if (data[i] != ITERS)
			errors++;
	if (errors) {
		printf("FAIL: %d counters wrong\n", errors);
		return 1;
	}

	double tx_per_sec = N_TX / sec;
	printf("mode=%s  tx=%d  iters=%d  total_ops=%d  time=%.4fs  tx/s=%.0f  PASS\n",
#ifndef MANUAL
	       "auto-wait",
#else
	       "manual-wait",
#endif
	       N_TX,
	       ITERS,
	       N_TX * ITERS,
	       sec,
	       tx_per_sec);
	return 0;
}
