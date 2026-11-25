// Version reclamation must respect live snapshots.
//
// A reader takes a snapshot, then keeps the transaction open while other
// threads commit thousands of versions (enough to trigger several version-GC
// sweeps).  Meanwhile the object it is reading gets a NEWER version, so the
// reader's snapshot still selects the older version node.  When it reads the
// object again at the end, it must observe exactly the same value.
//
// Bug this catches (docs/CORRECTNESS_FIXES.md §27, CSMV): the GC derived its
// low-water mark from `clock - 1000` instead of the oldest live snapshot, and
// trimmed every node with `timestamp < lwm`, which also dropped the *floor*
// version — the newest one a reader at the low-water mark still needs.  A live
// reader then either walked into a freed node or fell off the end of the chain
// and returned 0 in place of a committed value.
//
// Note that reading a version older than an object's head makes a commit fail
// validation on the OCC backends, which is expected and tolerated here: the
// check is on the value the reader OBSERVED, not on commit success.
#include "test_helpers.hpp"

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

static constexpr int NWORKERS = 4;
static constexpr int NKEYS = 64;
static constexpr uint64_t CHURN = 6000; // > 4 x CSMV_GC_INTERVAL(1024)

static std::atomic<int> reader_in_txn{0};
static std::atomic<uint64_t> churn_commits{0};
static std::atomic<int> stop{0};

static volatile uint64_t *target;
static volatile uint64_t *keys;
static volatile uint64_t observed_first = 0;
static volatile uint64_t observed_second = 0;
// Set only once the second read actually completed. Encounter-time-locking
// backends abort the reader when it re-reads a location another transaction has
// written since its snapshot, so "never got to observe again" is a legitimate
// outcome there — and must not be confused with observing a bogus value.
static volatile bool second_observed = false;

// One transaction attempt, abort tolerated (unlike tm_transaction(), which
// retries until commit and would spin forever on a guaranteed-stale read-set).
static void tm_attempt_once(const std::function<void()> &body)
{
	tm_longjmp_ret = sigsetjmp(tm_jmpbuf, 0);
	if (tm_longjmp_ret != 0)
		return; // aborted: the body's observations stand, the commit did not
	tm_begin();
	body();
	tm_end();
}

int main()
{
	printf("GC vs live reader — a snapshot's version must survive the churn\n");

	tm_init();
	tm_set_num_threads(2 + NWORKERS);
	tm_init_thread();
	tm_nested_call_counter++;

	target = (volatile uint64_t *)tm_malloc(sizeof(uint64_t));
	keys = (volatile uint64_t *)tm_malloc(NKEYS * sizeof(uint64_t));
	tm_transaction([&]() {
		tm_write_i8((uint64_t *)target, 1);
		for (int k = 0; k < NKEYS; k++)
			tm_write_i8((uint64_t *)&keys[k], 0);
	});

	uint64_t first = 0;
	tm_transaction([&]() { first = tm_read_i8((uint64_t *)target); });

	// Churn workers: unrelated objects, so nothing else conflicts with the
	// reader.  Their commits drive the clock (and the GC sweeps) forward.
	std::vector<std::thread> workers;
	for (int w = 0; w < NWORKERS; w++) {
		workers.emplace_back([w]() {
			tm_init_thread();
			tm_nested_call_counter++;
			while (!stop.load(std::memory_order_relaxed)) {
				tm_transaction([&]() {
					for (int k = 0; k < 8; k++) {
						int i = (w * 8 + k) % NKEYS;
						tm_write_i8((uint64_t *)&keys[i],
						            tm_read_i8((uint64_t *)&keys[i]) + 1);
					}
				});
				churn_commits.fetch_add(1, std::memory_order_relaxed);
			}
			tm_nested_call_counter--;
			tm_exit_thread();
		});
	}

	// The reader takes its snapshot while target has exactly one version...
	std::thread reader([&]() {
		tm_init_thread();
		tm_nested_call_counter++;
		while (churn_commits.load(std::memory_order_relaxed) < 50)
			std::this_thread::yield(); // let the churn (and GC) get going
		tm_attempt_once([&]() {
			observed_first = tm_read_i8((uint64_t *)target);
			reader_in_txn.store(1, std::memory_order_release);
			while (churn_commits.load(std::memory_order_relaxed) < CHURN)
				std::this_thread::yield();
			observed_second = tm_read_i8((uint64_t *)target);
			second_observed = true;
		});
		tm_nested_call_counter--;
		tm_exit_thread();
	});

	// ...and only after the snapshot is taken does target gain a newer version,
	// so the reader's version is no longer the head: it is the floor that GC
	// must keep.
	while (!reader_in_txn.load(std::memory_order_acquire))
		std::this_thread::yield();
	tm_transaction([&]() { tm_write_i8((uint64_t *)target, 2); });

	reader.join();
	stop.store(1);
	for (auto &t : workers)
		t.join();

	// The strict repeatable-read expectation is the multi-version contract:
	// CSMV serves reads from version chains, so its reader must keep observing
	// the version its snapshot selected. The lock-based backends in this suite
	// (TL2, TinySTM, NOrec, SwissTM, ...) instead detect the stale re-read at
	// commit time, so for them the test only requires that the reader never
	// sees a value that was never committed (no torn/garbage/freed read).
#ifdef TM_BACKEND_CSMV
	// Multi-version reads never abort: CSMV must serve the same version twice.
	bool ok = second_observed && observed_first == first && observed_second == first;
	printf("  reader saw %llu then %llu (expected %llu both times: MVCC snapshot), "
	       "churn=%llu commits\n",
	       (unsigned long long)observed_first,
	       (unsigned long long)observed_second,
	       (unsigned long long)first,
	       (unsigned long long)churn_commits.load());
#else
	// Lock-based backends detect the stale re-read at commit time (or abort the
	// reader at the read), so all they must guarantee here is that the reader
	// never observes a value that was never committed.
	bool ok = (observed_first == first) &&
	          (!second_observed || observed_second == first || observed_second == 2);
	printf("  reader saw %llu%s%llu (expected %llu, then %llu or 2, or an abort "
	       "before re-observing), churn=%llu commits\n",
	       (unsigned long long)observed_first,
	       second_observed ? " then " : " (aborted before re-read, last=)",
	       (unsigned long long)observed_second,
	       (unsigned long long)first,
	       (unsigned long long)first,
	       (unsigned long long)churn_commits.load());
#endif
	tm_nested_call_counter--;
	tm_exit_thread();
	tm_exit();

	printf("%s\n", ok ? "Result: PASS" : "Result: FAIL");
	return ok ? 0 : 1;
}
