// ec_bank_bench.cpp — measured throughput of the EC bank (Ch. 7, §7.7),
// the C++ counterpart of research/ec-bank/ec_bank_poc.py.
//
// The book's EC column of Table tab:ec-vs-stm was analytical; this makes
// it measured.  Same workload shape as benchmarks/cpp/bank: A accounts,
// transfers of 1 unit between random pairs, N total ops.  The protocol is
// the EC design of research/ec-bank/:
//
//   commit  = append {id, src, dst, amt} to a thread-private, grow-only
//             log (zero coordination: no shared state, no validation);
//   view    = fold of the applied op-set, maintained incrementally
//             (apply(op): bal[src] -= amt; bal[dst] += amt — one rule for
//             commits and compensations alike, amt<0 for the latter);
//   delivery= periodic gossip rounds merging peers' published logs
//             (union by id: idempotent, commutative — CRDT merge);
//   repair  = after each merge, every negative account triggers the
//             compensation (same src/dst, negated amt) of the max-id
//             uncompensated debit op on that account; the culprit rule is
//             a pure function of the applied set, so replicas agree.
//
// Modes:
//   --commit-only   append-only upper bound (no merge/fold/repair): the
//                   "zero-coordination commit" the EC column promised.
//   --full          the whole protocol, one thread per replica; this is
//                   where the conservation-of-overhead list of §7.7 shows
//                   up in the numbers.
//
// Checks at quiescence (gossip to fixpoint, then repair rounds): views
// are SEC-equal across replicas, money is conserved, no balance negative.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Op {
	uint64_t id;     // totally ordered: (seq << 8) | replica
	uint64_t parent; // compensation ops: culprit id; 0 for commits.
	                 // Compensation id is derived from parent (bit 63), so
	                 // replicas that independently detect the same violation
	                 // emit IDENTICAL ops and the CRDT dedup makes the
	                 // repair exactly-once globally.
	uint32_t src, dst;
	int32_t amt; // compensation: same src/dst, amt < 0
};

struct Config {
	int accounts = 1024;
	int threads = 4;
	uint64_t ops = 20000;       // total transfers across all replicas
	int merge_period = 64;      // commits between merge/repair rounds
	int initial_balance = 1000; // small values force transient negatives
	bool full = true;
};

static int64_t INITIAL_BALANCE = 1000;

static Config g_cfg; // read by Replica::init for the append-stability reserve

struct Replica {
	std::vector<Op> log;                // own ops (thread-private)
	std::atomic<uint64_t> published{0}; // how many of log[] peers may read
	std::vector<Op> applied;            // merged op-set (deduped by id)
	std::unordered_set<uint64_t> applied_ids;
	std::unordered_set<uint64_t> parent_ids; // culprit ids already repaired
	std::vector<std::set<uint64_t>> debits;  // per account: live debit op ids
	std::unordered_map<uint64_t, Op> by_id;  // culprit lookup
	std::vector<uint64_t> cursor;            // applied prefix per producer
	std::vector<int64_t> balance;            // incremental fold accumulator
	uint64_t seq = 0;                        // own id counter
	std::atomic<uint64_t> n_commits{0}, n_compensations{0};
	std::atomic<uint64_t> fold_ops{0}; // op-records applied to the fold
	void init(int accounts)
	{
		// Append-stability contract: peers read log[] up to `published`
		// concurrently with push_back, so the vector must NEVER reallocate
		// — reserve the upper bound (ops + headroom for compensations).
		log.reserve(g_cfg.ops + 4096);
		(void)accounts;
		balance.assign(accounts, INITIAL_BALANCE);
		debits.resize(accounts);
		cursor.assign(64, 0); // up to 64 replicas
	}
	uint64_t next_id(int rid) { return (++seq << 8) | (uint64_t)rid; }
};

static std::atomic<bool> g_start{false}, g_stop{false};
static std::atomic<uint64_t> g_ticket{0};

// the one fold rule, for commits and compensations alike
static inline void apply_op(Replica &r, const Op &o)
{
	r.balance[o.src] -= o.amt;
	r.balance[o.dst] += o.amt;
	if (o.amt > 0) {
		r.debits[o.src].insert(o.id);
		r.by_id[o.id] = o;
	} else {
		r.parent_ids.insert(o.parent); // global, exactly-once repair marker
	}
}

// merge every producer's published log into r's applied set (CRDT union)
static bool merge_round(Replica &r, std::vector<Replica *> &all)
{
	bool grew = false;
	for (int p = 0; p < (int)all.size(); ++p) {
		Replica &pr = *all[p];
		uint64_t n = pr.published.load(std::memory_order_acquire);
		uint64_t &cur = r.cursor[p];
		for (; cur < n; ++cur) {
			Op o = pr.log[cur];
			if (r.applied_ids.insert(o.id).second) {
				r.applied.push_back(o);
				apply_op(r, o);
				r.fold_ops.fetch_add(1, std::memory_order_relaxed);
				grew = true;
			}
		}
	}
	return grew;
}

// detection + repair over the applied set (max-id uncompensated culprit;
// O(log n) culprit selection via per-account ordered debit sets)
static bool repair_pass(Replica &r, int rid)
{
	bool did = false;
	for (size_t a = 0; a < r.balance.size(); ++a) {
		while (r.balance[a] < 0) {
			const Op *culprit = nullptr;
			auto &ds = r.debits[a];
			for (auto it = ds.rbegin(); it != ds.rend(); ++it) {
				if (r.parent_ids.count(*it))
					continue; // repaired globally
				culprit = &r.by_id.at(*it);
				break;
			}
			if (!culprit)
				break; // initial deficit: nothing to compensate
			Op c{culprit->id | (1ULL << 63),
			     culprit->id,
			     culprit->src,
			     culprit->dst,
			     -culprit->amt}; // deterministic: id from parent
			r.log.push_back(c);  // broadcast: a compensation is a new op
			r.published.store(r.log.size(), std::memory_order_release);
			r.n_compensations.fetch_add(1, std::memory_order_relaxed);
			if (r.applied_ids.insert(c.id).second) {
				r.applied.push_back(c);
				apply_op(r, c);
			}
			r.cursor[rid] = r.log.size();
			did = true;
		}
	}
	return did;
}

static void worker(Replica *r, int rid, std::vector<Replica *> *all)
{
	std::mt19937 rng(0x5eed + rid);
	std::uniform_int_distribution<int> ad(0, g_cfg.accounts - 1);
	while (!g_start.load(std::memory_order_acquire))
		std::this_thread::yield();
	while (!g_stop.load(std::memory_order_relaxed)) {
		if (g_ticket.fetch_add(1, std::memory_order_relaxed) >= g_cfg.ops) {
			g_stop.store(true, std::memory_order_relaxed);
			break;
		}
		int src = ad(rng), dst = ad(rng);
		if (dst == src)
			dst = (src + 1) % g_cfg.accounts;
		Op o{r->next_id(rid), 0 /*parent*/, (uint32_t)src, (uint32_t)dst, 1};
		r->log.push_back(o); // THE COMMIT: append, zero coordination
		r->published.store(r->log.size(), std::memory_order_release);
		r->n_commits.fetch_add(1, std::memory_order_relaxed);
		if (g_cfg.full && (r->seq % g_cfg.merge_period) == 0) {
			merge_round(*r, *all);
			repair_pass(*r, rid);
		}
	}
	r->published.store(r->log.size(), std::memory_order_release);
}

// gossip to fixpoint, then repair rounds until every view is safe
static void quiesce(std::vector<Replica *> &all)
{
	for (;;) {
		bool grew = false;
		for (auto *r : all)
			grew |= merge_round(*r, all);
		bool repaired = false;
		for (int i = 0; i < (int)all.size(); ++i)
			repaired |= repair_pass(*all[i], i);
		if (!grew && !repaired)
			break;
	}
}

int main(int argc, char **argv)
{
	Config c;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc)
			c.threads = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-a") && i + 1 < argc)
			c.accounts = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			c.ops = atoll(argv[++i]);
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			c.merge_period = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			c.initial_balance = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--commit-only"))
			c.full = false;
		else {
			fprintf(stderr,
			        "usage: %s [-t T] [-a A] [-n N] [-p P] [--commit-only]\n",
			        argv[0]);
			return 2;
		}
	}
	g_cfg = c;
	INITIAL_BALANCE = c.initial_balance;
	std::vector<Replica> reps(c.threads);
	std::vector<Replica *> all;
	for (int i = 0; i < c.threads; ++i) {
		reps[i].init(c.accounts);
		all.push_back(&reps[i]);
	}

	std::vector<std::thread> ts;
	for (int i = 0; i < c.threads; ++i)
		ts.emplace_back(worker, &reps[i], i, &all);
	auto t0 = std::chrono::steady_clock::now();
	g_start.store(true, std::memory_order_release);
	for (auto &t : ts)
		t.join();
	auto t1 = std::chrono::steady_clock::now();

	double secs = std::chrono::duration<double>(t1 - t0).count();
	uint64_t commits = 0, comps = 0, folds = 0, applied = 0;
	for (auto &r : reps) {
		commits += r.n_commits.load();
		comps += r.n_compensations.load();
		folds += r.fold_ops.load();
		applied += r.applied.size();
	}
	printf("EC bank: threads=%d accounts=%d ops=%llu mode=%s merge_period=%d "
	       "init_balance=%d\n",
	       c.threads,
	       c.accounts,
	       (unsigned long long)c.ops,
	       c.full ? "full" : "commit-only",
	       c.merge_period,
	       c.initial_balance);
	printf("  throughput: %.3f Mops/s (%.3fs wall, %llu commits)\n",
	       commits / secs / 1e6,
	       secs,
	       (unsigned long long)commits);
	if (c.full)
		printf("  overhead: %llu compensations, %llu op-records folded, "
		       "avg applied-set %.0f ops/replica\n",
		       (unsigned long long)comps,
		       (unsigned long long)folds,
		       (double)applied / c.threads);

	quiesce(all);
	int64_t conserved = c.accounts * INITIAL_BALANCE;
	bool sec = true, safe = true, cons_ok = true;
	for (int i = 1; i < c.threads; ++i)
		if (reps[i].balance != reps[0].balance)
			sec = false;
	for (auto *r : all) {
		int64_t sum = 0;
		for (int64_t b : r->balance) {
			sum += b;
			if (b < 0)
				safe = false;
		}
		if (sum != conserved)
			cons_ok = false;
	}
	printf("  quiescence: SEC=%s conserved=%s non-negative=%s\n",
	       sec ? "yes" : "NO",
	       cons_ok ? "yes" : "NO",
	       safe ? "yes" : "NO");
	return (sec && safe && cons_ok) ? 0 : 1;
}
