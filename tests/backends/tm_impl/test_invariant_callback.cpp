// test_invariant_callback.cpp — the user-oracle hook API, end to end.
//
// tm_register_invariant_callback() (backends/tm_impl/common/tm_hooks.cpp:842) swaps the
// public tm_end function pointer for a wrapper that commits and then calls the oracle,
// printing `TM-INVARIANT: FAIL (…)` to stderr when the oracle returns non-zero
// (tm_hooks.cpp:823-836). Nothing in the tree had ever called it: the wrapper rewrites a
// hook every backend depends on, and tools/stm_bug_tool/invariant_checker.py's
// `user_oracle` invariant parses a line no test produced. Both ends are pinned here — the
// C++ side by the counts below, the tool side by the `oracle_callback_failure` fixture in
// tools/stm_bug_tool/selftest.py, which uses this exact format string.
//
// Single-threaded on purpose: what is under test is the hook wrapper (is the oracle called
// exactly once per commit, and only for commits?), not scheduling, and every one of the 12
// backends in this suite has to agree on the answer.
//
// One field of the printed line is not meaningful here: `after tx %llu` prints
// `t_current_tx_id`, which tm_hooks.cpp increments only on the trace path (:344, under
// TM_TRACE_PATH), so it reads 0 in a plain unit test. This test therefore pins the prefix
// the checker matches (`TM-INVARIANT: FAIL`), not that field — see TODO.md.
//
// A money-conservation oracle over two accounts, as in benchmarks/cpp/fuzz-bank: a
// transaction that preserves the total must produce no FAIL line, and one that creates
// money must produce exactly one.

#include "test_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static constexpr int kCleanTxns = 20;
static constexpr uint64_t kStartA = 1000;
static constexpr uint64_t kStartB = 1000;
static constexpr int64_t kTotal = (int64_t)kStartA + (int64_t)kStartB;
static constexpr uint64_t kTransfer = 1;

// The oracle's subject: two accounts, moved between only inside transactions.
static volatile uint64_t *g_acct_a;
static volatile uint64_t *g_acct_b;

// Oracle bookkeeping. The callback runs after a successful commit, on the committing
// thread, so plain counters are enough for a single-threaded test.
static int g_oracle_calls = 0;
static int g_oracle_failures = 0;
static int64_t g_last_sum = 0;

static int money_conserved(void *)
{
	g_oracle_calls++;
	int64_t sum = (int64_t)g_acct_a[0] + (int64_t)g_acct_b[0];
	g_last_sum = sum;
	if (sum != kTotal) {
		g_oracle_failures++;
		return 1; // non-zero ⇒ the wrapper reports a FAIL for this commit
	}
	return 0;
}

// ── stderr capture ─────────────────────────────────────────────────
// The wrapper prints to stderr, so the test has to read its own stderr. Redirect to a
// temp file for one phase, then count the needle in it.
struct StderrCapture {
	int saved_fd = -1;
	char path[64];

	explicit StderrCapture(const char *name)
	{
		snprintf(path, sizeof(path), "/tmp/llvm-tm-invcb-%s-%d.log", name, (int)getpid());
		fflush(stderr);
		saved_fd = dup(fileno(stderr));
		if (freopen(path, "w", stderr) == nullptr)
			perror("freopen");
	}

	~StderrCapture() { finish(); }

	int finish()
	{
		if (saved_fd < 0)
			return -1;
		fflush(stderr);
		dup2(saved_fd, fileno(stderr));
		close(saved_fd);
		saved_fd = -1;

		FILE *f = fopen(path, "r");
		if (!f)
			return -1;
		int hits = 0;
		char *line = nullptr;
		size_t cap = 0;
		ssize_t len;
		while ((len = getline(&line, &cap, f)) != -1) {
			// The exact substring invariant_checker.py's check_user_oracle looks for.
			if (strstr(line, "TM-INVARIANT: FAIL") != nullptr)
				hits++;
		}
		free(line);
		fclose(f);
		return hits;
		// The capture file is deliberately left in place: it holds the wrapper's own
		// output verbatim, which is the input invariant_checker.py's user_oracle
		// invariant consumes (see main's closing note for the path).
	}
};

// ── Transactions ───────────────────────────────────────────────────
// Move `amount` from A to B. Reads and writes go through the hook table like every other
// test here, so the commit is a normal backend commit.
static void transfer(uint64_t amount)
{
	tm_transaction([&]() {
		uint64_t a = tm_r8((uint64_t *)g_acct_a);
		uint64_t b = tm_r8((uint64_t *)g_acct_b);
		tm_w8((uint64_t *)g_acct_a, a - amount);
		tm_w8((uint64_t *)g_acct_b, b + amount);
	});
}

// Creates money: the total after this commit is kTotal + kTransfer, which the oracle must
// catch. This is the point of the test, not a backend misbehaviour.
static void print_money(uint64_t amount)
{
	tm_transaction([&]() {
		uint64_t b = tm_r8((uint64_t *)g_acct_b);
		tm_w8((uint64_t *)g_acct_b, b + amount);
	});
}

static int g_rc = 0;

static void check(const char *name, bool ok, int64_t got, int64_t expected)
{
	g_rc |= check_result({name, ok, got, expected});
}

int main()
{
	printf("Invariant callback — tm_register_invariant_callback() wrapper\n");
	printf("Clean transactions: %d (transfer %llu each), then one that prints money\n\n",
	       kCleanTxns,
	       (unsigned long long)kTransfer);

	tm_init();
	tm_init_thread();
	tm_nested_call_counter++;

	g_acct_a = (volatile uint64_t *)tm_malloc(sizeof(uint64_t));
	g_acct_b = (volatile uint64_t *)tm_malloc(sizeof(uint64_t));
	g_acct_a[0] = kStartA;
	g_acct_b[0] = kStartB;

	// Registering must replace the public hook. If a backend installed its real commit
	// after this point, g_original_tm_end would have captured the wrong pointer and the
	// counts below would come out zero — which is the other thing this pins.
	void (*commit_hook_before)() = tm_end;
	tm_register_invariant_callback(money_conserved, nullptr);
	check("wrapper_installed",
	      tm_end != commit_hook_before,
	      tm_end != commit_hook_before,
	      1);

	// Phase 1: transactions that preserve the total. The oracle runs once per commit and
	// never reports.
	int clean_hits;
	{
		StderrCapture cap("clean");
		for (int i = 0; i < kCleanTxns; ++i)
			transfer(kTransfer);
		clean_hits = cap.finish();
	}
	check("oracle_calls_per_commit",
	      g_oracle_calls == kCleanTxns,
	      g_oracle_calls,
	      kCleanTxns);
	check("clean_run_total", g_last_sum == kTotal, g_last_sum, kTotal);
	check("clean_run_findings", clean_hits == 0, clean_hits, 0);

	// Phase 2: one transaction that creates money. The commit succeeds — the oracle is a
	// post-commit check, not a conflict — so it must fire once and print one FAIL line.
	int dirty_hits;
	int calls_before = g_oracle_calls;
	char verdict_path[64];
	{
		StderrCapture cap("dirty");
		print_money(kTransfer);
		dirty_hits = cap.finish();
		snprintf(verdict_path, sizeof(verdict_path), "%s", cap.path);
	}
	check("oracle_called_for_violation",
	      g_oracle_calls - calls_before == 1,
	      g_oracle_calls - calls_before,
	      1);
	check("violation_counted", g_oracle_failures == 1, g_oracle_failures, 1);
	check("violation_reported", dirty_hits == 1, dirty_hits, 1);

	printf("\n  (final sum %lld, expected %lld — the violation is deliberate)\n",
	       (long long)g_last_sum,
	       (long long)kTotal);
	// The oracle verdict is only ever written inside the capture window (the wrapper runs
	// during the commit), so this file — not the process's own stderr — is what a log
	// tool consumes. Feed it to the checker to see the whole path close:
	//   python3 tools/stm_bug_tool/invariant_checker.py --log <path>
	// → "RESULT CRITICAL: 1 critical" and exit 1.
	printf("  oracle verdict captured in: %s\n", verdict_path);

	tm_nested_call_counter--;
	tm_exit_thread();
	tm_exit();
	return g_rc;
}
