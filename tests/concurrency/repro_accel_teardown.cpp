/**
 * repro_accel_teardown.cpp — the shared guard table must survive a
 * tm_exit()/tm_init() cycle.
 *
 * accel::shutdown() runs from tm_exit() and releases the fine-grain-SVM block
 * that holds TL2's guard table, but two things keep it dangling:
 * tl2::STM::g_guards is never rebound, and STM::initialized stays true, so a
 * later init() neither re-probes nor re-binds a table. Every transaction of the
 * next phase then runs against memory the device driver owns again, and the
 * accelerator is silently disarmed for the rest of the process — which is what
 * any process with more than one init/exit phase (the phase-switch tests, an
 * embedded runtime that restarts its transactional section) actually does.
 *
 * The observable is the dispatch counter, not a crash: with prevalidation
 * forced on (TM_ACCEL_MIN=1) every committing transaction dispatches a kernel,
 * so a phase that dispatches nothing is a phase that is not using the shared
 * table. Each phase also re-verifies its data, because a guard table that points
 * at stale memory can equally show up as a wrong value or as a commit that can
 * never acquire its guard; the retry loop is therefore bounded and reports
 * livelock instead of hanging.
 *
 * Needs a host with a fine-grain-SVM device: without one there is no device path
 * to test, and the program reports SKIP and exits 0.
 *
 * usage: TM_ACCEL_MIN=1 repro_accel_teardown   exit 0 clean/SKIP, 2 defect
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "expli_tm_api/tm_api.hpp"

#ifdef TM_ACCEL_GPU
#include "accel_probe.hpp"
#endif

extern __thread int tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

namespace
{

constexpr int N = 64;
constexpr int MAX_RETRIES = 20000;

int failures = 0;

void fail(const char *what, long long got, long long want)
{
	std::printf("FAIL %s: got %lld want %lld\n", what, got, want);
	++failures;
}

// One transaction with a bounded number of abort retries. Returns false if the
// transaction could not commit (which, for these uncontended single-threaded
// bodies, means a guard that never became acquirable).
bool tx_run(const std::function<void()> &body)
{
	for (int retry = 0; retry < MAX_RETRIES; retry++) {
		if (sigsetjmp(tm_jmpbuf, 0) != 0) {
			tm_nested_call_counter = 0; // aborted back here
			continue;
		}
		tm_nested_call_counter = 1;
		tm_set_jmpbuf(&tm_jmpbuf);
		tm_begin();
		body();
		tm_end();
		tm_nested_call_counter = 0;
		return true;
	}
	return false;
}

// Allocates a fresh array from the TM region (tm_exit() destroys the previous
// phase's region), writes a known value into every element, then does one
// read-modify-write per element — the RMW is what forces each commit to acquire
// a guard and to dispatch a prevalidation. Verifies the results.
void run_phase(int phase)
{
	int *a = (int *)expli::TM<int>::malloc(N * sizeof(int));
	if (!a) {
		fail("phase region allocation returned null", 0, 1);
		return;
	}
	for (int i = 0; i < N; i++) {
		int want = phase * 1000 + i;
		if (!tx_run([&, want] { expli::TM<int>::write_at(&a[i], want); })) {
			fail("phase write could not commit (guard never acquirable)", 0, 1);
			return;
		}
	}
	for (int i = 0; i < N; i++) {
		if (!tx_run([&] {
			    int v = expli::TM<int>::read_at(&a[i]);
			    expli::TM<int>::write_at(&a[i], v + 1);
		    })) {
			fail("phase read-modify-write could not commit", 0, 1);
			return;
		}
	}
	for (int i = 0; i < N; i++) {
		long long got = 0;
		if (!tx_run([&, i] { got = expli::TM<int>::read_at(&a[i]); })) {
			fail("phase read could not commit", 0, 1);
			return;
		}
		long long want = phase * 1000 + i + 1;
		if (got != want)
			fail("phase value", got, want);
		if ((long long)a[i] != want)
			fail("committed memory", (long long)a[i], want);
	}
}

} // namespace

int main()
{
#ifndef TM_ACCEL_GPU
	std::printf("SKIP: built without the accelerator path (BACKEND != ACCELTL2)\n");
	return 0;
#else
	// Force a device dispatch on every committing transaction; at the default
	// threshold the accelerator idles on a read set this small and the test
	// would pass (or fail) for entirely the wrong reason.
	const char *min = getenv("TM_ACCEL_MIN");
	if (!min || !*min || min[0] == '0') {
		std::printf("SKIP: rerun with TM_ACCEL_MIN=1 so that every commit "
		            "dispatches\n");
		return 0;
	}

	expli::TM<int>::init();
	expli::TM<int>::thread_init();

	// Checked after init() because the guard table is allocated by the backend's
	// init, and only the backend knows whether it landed in shared memory.
	if (!accel::probe_available() || !accel::guards_shared()) {
		std::printf("SKIP: no fine-grain-SVM device — no shared guard table whose "
		            "teardown can be tested\n");
		return 0;
	}

	long long before = (long long)accel::dispatch_count();
	run_phase(0);
	long long phase1 = (long long)accel::dispatch_count() - before;

	// The teardown under test: full teardown, then a fresh init. Phase 2 builds
	// its own state because tm_exit() destroys the TM region.
	tm_exit_thread();
	tm_exit();
	tm_init();
	tm_init_thread();

	before = (long long)accel::dispatch_count();
	run_phase(1);
	long long phase2 = (long long)accel::dispatch_count() - before;

	std::printf("device prevalidations dispatched: phase-1=%lld phase-2=%lld\n",
	            phase1,
	            phase2);

	if (phase1 <= 0)
		fail("phase-1 dispatches (accelerator never engaged)", phase1, 1);
	if (phase2 <= 0)
		fail("phase-2 dispatches (tm_exit disarmed the accelerator for good)", phase2, 1);

	if (failures) {
		std::printf("repro_accel_teardown: %d FAILURES\n", failures);
		return 2;
	}
	std::printf("repro_accel_teardown: PASS\n");
	return 0;
#endif
}
