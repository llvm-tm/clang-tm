// bank_si.cpp — the book's Ch. 7 anomaly gallery, run live on a toy bank.
//
// Standalone teaching demo (no TM backend required):
//
//   make -C benchmarks/cpp bin/bank_si && ./bin/bank_si
//
// A ~100-line educational runtime `minitm` implements four isolation levels
// over a bank of named accounts:
//
//   RU     reads may see other transactions' PENDING (uncommitted) writes
//   RC     reads see fresh committed state; each read picks its own snapshot
//   SI     reads come from one snapshot taken at begin; commit validates the
//          WRITE-set only against the snapshot (first-committer-wins, WW) —
//          predicates are NOT validated
//   OPAQUE SI plus read-set revalidation at commit: the optimistic
//          serializability rule of NOrec/TL2 (the book's default end)
//
// Five scenarios, each run at its two instructive levels, with the
// interleaving FORCED by barriers so the outcomes are deterministic — the
// anomalies are shown, not raced for.  The write-skew scenario is the TLA+
// model docs/proofs/BankSI.tla (constants 12/8/16, config BankSI-demo-skew)
// executed as C++; BankWeak.tla mirrors the RU/RC scenarios.
//
// Every scenario prints the history in the book's notation
// (r_T(x)->v, w_T(x<-v)) and a VERDICT line.

#include "expli_tm_api/thread_barrier.hpp"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace minitm
{

enum Level { RU, RC, SI, OPAQUE };

static const char *level_name(Level l)
{
	switch (l) {
	case RU: return "RU";
	case RC: return "RC";
	case SI: return "SI";
	case OPAQUE: return "OPAQUE (serializable OCC)";
	}
	return "?";
}

struct Store
{
	std::mutex m;
	std::map<std::string, int> committed;    // the committed state
	std::map<std::string, long> version;     // per-object commit counter
	std::map<std::string, int> pending;      // RU: exposed uncommitted writes

	int fresh(const std::string &k) { return committed.at(k); }
};

class Tx
{
	Store *s;
	Level lvl;
	std::string name;
	std::map<std::string, int> reads;      // obj -> value read (snapshot for SI/OPAQUE)
	std::map<std::string, long> rver;      // obj -> committed version at read
	std::map<std::string, int> writes;     // obj -> value to install

public:
	Tx(Store *s, Level lvl, std::string name)
	    : s(s), lvl(lvl), name(std::move(name))
	{
	}

	int read(const std::string &k)
	{
		std::lock_guard<std::mutex> g(s->m);
		if (lvl == SI || lvl == OPAQUE) {
			auto it = reads.find(k);
			if (it != reads.end()) { // snapshot: answer from cache
				printf("  %s: r(%s)->%d   [snapshot]\n", name.c_str(),
				       k.c_str(), it->second);
				return it->second;
			}
			int v = s->committed.at(k);
			reads[k] = v;
			rver[k] = s->version.at(k);
			printf("  %s: r(%s)->%d\n", name.c_str(), k.c_str(), v);
			return v;
		}
		if (lvl == RU) {
			auto pit = s->pending.find(k);
			int v = pit != s->pending.end() ? pit->second
			                                : s->committed.at(k);
			printf("  %s: r(%s)->%d%s\n", name.c_str(), k.c_str(), v,
			       pit != s->pending.end() ? "   [DIRTY: uncommitted!]" : "");
			if (pit == s->pending.end()) {
				reads[k] = v; // bookkeeping for the trace only
				rver[k] = s->version.at(k);
			}
			return v;
		}
		// RC: fresh committed value, every read its own snapshot
		int v = s->committed.at(k);
		printf("  %s: r(%s)->%d\n", name.c_str(), k.c_str(), v);
		reads[k] = v;
		rver[k] = s->version.at(k);
		return v;
	}

	void write(const std::string &k, int v)
	{
		std::lock_guard<std::mutex> g(s->m);
		writes[k] = v;
		printf("  %s: w(%s<-%d)\n", name.c_str(), k.c_str(), v);
		if (lvl == RU)
			s->pending[k] = v; // RU: uncommitted writes are exposed
	}

	bool commit()
	{
		std::lock_guard<std::mutex> g(s->m);
		if (lvl == SI || lvl == OPAQUE) {
			// WW test: a write conflicts if the object changed since our
			// snapshot read of it (first committer wins).
			for (auto &w : writes) {
				long snap = rver.count(w.first)
				                ? rver.at(w.first)
				                : s->version.at(w.first);
				if (s->version.at(w.first) != snap) {
					printf("  %s: ABORT at commit — %s was written after our "
					       "snapshot\n",
					       name.c_str(), w.first.c_str());
					return false;
				}
			}
			if (lvl == OPAQUE) {
				// read-set revalidation: the term the SI rule lacks
				for (auto &r : reads) {
					if (s->version.at(r.first) != rver.at(r.first)) {
						printf("  %s: ABORT at commit — read-set stale on %s "
						       "(predicate revalidated)\n",
						       name.c_str(), r.first.c_str());
						return false;
					}
				}
			}
		}
		for (auto &w : writes) {
			s->committed[w.first] = w.second;
			s->version[w.first]++;
		}
		for (auto &w : writes)
			s->pending.erase(w.first);
		printf("  %s: COMMIT\n", name.c_str());
		return true;
	}

	void abort()
	{
		std::lock_guard<std::mutex> g(s->m);
		for (auto &w : writes)
			s->pending.erase(w.first);
		printf("  %s: ABORT (voluntary)\n", name.c_str());
	}

	// RU+nonatomic: publish writes one at a time (the non-atomic commit).
	void publish_one(const std::string &k)
	{
		std::lock_guard<std::mutex> g(s->m);
		s->committed[k] = writes.at(k);
		s->version[k]++;
	}
};

static int dump(Store &s, const std::string &k)
{
	std::lock_guard<std::mutex> g(s.m);
	return s.committed.at(k);
}

// An atomic view of two accounts: both values read under ONE lock, so
// the observer's sum is a single instantaneous snapshot, not a torn
// pair of separately-locked reads.
static std::pair<int, int> dump2(Store &s, const std::string &a,
                                 const std::string &b)
{
	std::lock_guard<std::mutex> g(s.m);
	return {s.committed.at(a), s.committed.at(b)};
}

} // namespace minitm

using namespace minitm;

static std::atomic<int> g_failures{0};

static void verdict(const char *scenario, const char *level, bool anomaly,
                    const char *what)
{
	printf("  VERDICT: %s at %s — %s %s\n", anomaly ? "ANOMALY" : "clean",
	       level, what, anomaly ? "OBSERVED" : "prevented");
	if (anomaly)
		g_failures++;
}

// ------------------------------------------------------------------
// Scenario 1 — write skew.  A = B = 12.  Each transaction transfers 8 of
// its own account to an external destination X1/X2, guarded by the
// predicate "after my draw the combined balance stays >= 16".  Each
// predicate passes alone (24 - 8 = 16); jointly they fail (24 - 16 = 8).
// Disjoint write-sets => SI's WW test sees no conflict.  This is the
// constant set of docs/proofs/BankSI.tla (BankSI-demo-skew.cfg).
// ------------------------------------------------------------------
static void scenario_write_skew(Level lvl)
{
	printf("\n[1] write skew @ %s   (A=B=12, two transfers of 8, guard "
	       "A+B-8 >= 16)\n",
	       level_name(lvl));
	Store st;
	st.committed["A"] = st.committed["B"] = 12;
	st.version["A"] = st.version["B"] = 0;

	expli::Barrier t1_done(2), t2_done(2);

	auto body = [&](const char *self, const char *other) {
		Tx tx(&st, lvl, self);
		int a = tx.read(self), b = tx.read(other);
		t1_done.wait(); // both transactions now hold their snapshots
		bool ok = a + b - 8 >= 16; // the predicate — on OUR snapshot
		printf("  %s: predicate %d+%d-8 >= 16 -> %s\n", self, a, b,
		       ok ? "TRUE, transfer" : "FALSE, skip");
		if (ok)
			tx.write(self, a - 8);
		if (ok)
			tx.commit();
		t2_done.wait();
	};
	std::thread t1(body, "A", "B"), t2(body, "B", "A");
	t1.join();
	t2.join();

	int sum = dump(st, "A") + dump(st, "B");
	printf("  final: A+B = %d (invariant A+B >= 16)\n", sum);
	bool violated = sum < 16;
	verdict("write skew", level_name(lvl), violated, "combined reserve");
}

// ------------------------------------------------------------------
// Scenario 2 — dirty read.  A = 12.  T1 debits 5 and ABORTS; T2 reads A
// in between.  RU answers with the pending value; RC never.
// (docs/proofs/BankWeak.tla, config BankWeak-ru.cfg.)
// ------------------------------------------------------------------
static void scenario_dirty_read(Level lvl)
{
	printf("\n[2] dirty read @ %s   (A=12; T1 debits 5 then aborts; T2 "
	       "reads A)\n",
	       level_name(lvl));
	Store st;
	st.committed["A"] = 12;
	st.version["A"] = 0;

	expli::Barrier wrote(2), read1(2), aborted(2);
	int seen = -1;

	std::thread t1([&] {
		Tx tx(&st, lvl, "T1");
		int a = tx.read("A");
		tx.write("A", a - 5);
		wrote.wait();      // let T2 look at the pending value
		read1.wait();
		tx.abort();        // the debit never happened
		aborted.wait();
	});
	std::thread t2([&] {
		wrote.wait();
		Tx tx(&st, lvl, "T2");
		seen = tx.read("A");
		read1.wait();
		tx.commit();
		aborted.wait();
	});
	t1.join();
	t2.join();

	printf("  final: A = %d; T2 saw A = %d%s\n", dump(st, "A"), seen,
	       seen == 7 ? "  <- money that never existed" : "");
	bool dirty = seen == 7;
	verdict("dirty read", level_name(lvl), dirty,
	        "read of an uncommitted write");
}

// ------------------------------------------------------------------
// Scenario 3 — non-repeatable read.  A = 12; T2 reads A, T1 deposits 4
// and commits, T2 re-reads A.  RC answers twice from two different
// snapshots; SI answers twice from one.
// (docs/proofs/BankWeak.tla, config BankWeak-rc.cfg.)
// ------------------------------------------------------------------
static void scenario_nonrepeatable(Level lvl)
{
	printf("\n[3] non-repeatable read @ %s   (T2 reads A, T1 commits +4, "
	       "T2 re-reads A)\n",
	       level_name(lvl));
	Store st;
	st.committed["A"] = 12;
	st.version["A"] = 0;

	expli::Barrier r1(2), deposited(2);
	int a1 = -1, a2 = -1;

	std::thread t1([&] {
		r1.wait();
		Tx tx(&st, lvl, "T1");
		int a = tx.read("A");
		tx.write("A", a + 4);
		tx.commit();
		deposited.wait();
	});
	std::thread t2([&] {
		Tx tx(&st, lvl, "T2");
		a1 = tx.read("A");
		r1.wait();
		deposited.wait();
		a2 = tx.read("A");
		tx.commit();
	});
	t1.join();
	t2.join();

	printf("  T2 saw A = %d then %d%s\n", a1, a2,
	       a1 != a2 ? "  <- the same transaction got two answers" : "");
	bool broke = a1 != a2;
	verdict("non-repeatable read", level_name(lvl), broke,
	        "snapshot stability");
}

// ------------------------------------------------------------------
// Scenario 4 — lost update.  A = 12; both transactions add 4 to A based
// on a read.  RC validates nothing, the second commit overwrites the
// first.  SI's WW test aborts the second.
// (docs/proofs/BankWeak.tla, NoLostUpdate predicate.)
// ------------------------------------------------------------------
static void scenario_lost_update(Level lvl)
{
	printf("\n[4] lost update @ %s   (A=12; two concurrent +4 into A)\n",
	       level_name(lvl));
	Store st;
	st.committed["A"] = 12;
	st.version["A"] = 0;

	expli::Barrier both_read(2);
	std::vector<int> committed_n{0, 0};

	auto body = [&](int i) {
		Tx tx(&st, lvl, i == 0 ? "T1" : "T2");
		int a = tx.read("A");
		both_read.wait(); // both now base their write on A=12
		tx.write("A", a + 4);
		if (tx.commit())
			committed_n[i] = 1;
	};
	std::thread t1(body, 0), t2(body, 1);
	t1.join();
	t2.join();

	int sum = dump(st, "A");
	int expected = 12 + 4 * (committed_n[0] + committed_n[1]);
	printf("  final: A = %d; %d deposit(s) committed, expected A = %d\n",
	       sum, committed_n[0] + committed_n[1], expected);
	bool lost = sum != expected;
	verdict("lost update", level_name(lvl), lost, "update accounting");
}

// ------------------------------------------------------------------
// Scenario 5 — fan-out.  A commit publishes its writes ONE AT A TIME
// (a broken publish, like EAGER_BROKEN's: mechanism without sandboxing).
// The transfer debits A and credits B; an observer between the two
// publishes sees money in flight — a state no transaction ever produced.
// With atomic publish the observer always sees both or neither.
// ------------------------------------------------------------------
static void scenario_fanout(bool atomic_commit)
{
	printf("\n[5] fan-out — %s commit publish (transfer 5 from A to B)\n",
	       atomic_commit ? "atomic" : "NON-ATOMIC");
	Store st;
	st.committed["A"] = st.committed["B"] = 12;
	st.version["A"] = st.version["B"] = 0;

	expli::Barrier half(2), seen(2), done(2);
	int observed_sum = -1;

	std::thread t1([&] {
		Tx tx(&st, RU, "T1"); // RU chosen only to reuse the pending map
		int a = tx.read("A"), b = tx.read("B");
		tx.write("A", a - 5);
		tx.write("B", b + 5);
		if (!atomic_commit)
			tx.publish_one("A"); // money has left A...
		half.wait();              // observer looks HERE
		seen.wait();              // ...and has finished looking
		if (!atomic_commit)
			tx.publish_one("B"); // ...and only now arrives in B
		else
			tx.commit();         // ...both accounts, one instant
		done.wait();
	});
	std::thread observer([&] {
		half.wait();
		auto ab = dump2(st, "A", "B"); // one instant: both or neither
		observed_sum = ab.first + ab.second;
		printf("  observer: A = %d, B = %d, A+B = %d\n", ab.first,
		       ab.second, observed_sum);
		seen.wait();
		done.wait();
	});
	t1.join();
	observer.join();

	printf("  final: A+B = %d; observer saw A+B = %d\n",
	       dump(st, "A") + dump(st, "B"), observed_sum);
	bool torn = observed_sum != 24;
	verdict("fan-out", atomic_commit ? "atomic publish" : "non-atomic",
	        torn, "atomicity of commit");
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	printf("== bank_si — Ch. 7 anomaly gallery on the toy bank ==\n");

	scenario_write_skew(SI);
	scenario_write_skew(OPAQUE);
	scenario_dirty_read(RU);
	scenario_dirty_read(RC);
	scenario_nonrepeatable(RC);
	scenario_nonrepeatable(SI);
	scenario_lost_update(RC);
	scenario_lost_update(SI);
	scenario_fanout(false);
	scenario_fanout(true);

	// The demo EXPECTS exactly five anomalies — write skew at SI, dirty
	// read at RU, non-repeatable read and lost update at RC, and fan-out
	// at the non-atomic publish.  Every stronger contrast must be clean.
	printf("\n== summary: %d anomaly observation(s) — expected 5 "
	       "(skew@SI, dirty@RU, nonrep@RC, lostup@RC, fan-out@nonatomic) "
	       "==\n",
	       g_failures.load());
	if (g_failures.load() != 5) {
		printf("DEMO FAILED: the level/verdict table is wrong\n");
		return 1;
	}
	printf("DEMO PASSED\n");
	return 0;
}
