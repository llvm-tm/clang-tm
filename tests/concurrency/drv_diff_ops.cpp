/**
 * drv_diff_ops.cpp — differential/self-verifying concurrency driver (T1)
 *
 * Goal: find TM backend bugs FAST by making every legal commit order
 * produce the SAME final state, so a single invariant replaces a
 * serialization oracle:
 *
 *   Every transactional write in this workload is a bounded ADDITIVE
 *   delta (counters, 8-byte slots, bytes; transfers are -k/+k pairs —
 *   still additive). Additive deltas commute, so for every object o:
 *
 *       final(o) == init(o) + sum of deltas of transactions that COMMITTED to o
 *
 *   regardless of commit ORDER. Each transaction records its delta list
 *   and flushes it into a plain-atomic SHADOW model ONLY AFTER tm_end()
 *   returns (return == commit; an abort siglongjmps and never reaches the
 *   flush). At quiescence the driver asserts equality per object.
 *   Violations = lost updates, write-buffer mis-flush, validation holes,
 *   release ordering bugs — classes unit tests and money-conservation miss.
 *
 * In-transaction checks:
 *   CHECK:    repeatable read (read v1, other reads, re-read v2) and
 *             |v - shadow| <= tol. These only hold for transactions that
 *             actually VALIDATE, so a CHECK txn performs a scratch write
 *             (some backends, e.g. TL2's ROCO, commit read-only txns
 *             WITHOUT validation — a legit non-repeatable read there).
 *             Verdicts are held thread-local and charged only when the txn
 *             commits (a correct OCC aborts an inconsistent history, and
 *             that abort siglongjmps past the charge).
 *   READOWN:  v = read; write v + k; read == v + k (incl. the i1 byte
 *             path). A mid-flight read-after-own-write mismatch is
 *             unambiguously wrong → charged immediately.
 *
 * Thread waves + short-lived-thread churn target TLS lifecycle bugs
 * (cf. the known CSMV "thread lifecycle" failure).
 *
 * THREE harness pitfalls found building this (all cost real debugging
 * time and are easy to reintroduce):
 *   1. Abort retries re-enter AT the sigsetjmp return point — per-attempt
 *      counters/setup must be AFTER it or aborts become invisible.
 *   2. The shadow flush must be AFTER tm_end(), not in the tx body —
 *      flushing inside the body charges deltas for transactions that then
 *      abort, corrupting the model (this is what made a correct TL2 "fail").
 *   3. Same trap one level up: per-attempt RESETS (the delta buffer) must
 *      also sit AFTER sigsetjmp — a clear() before it is skipped by the
 *      retry, so each retry appends onto the abandoned attempt's deltas
 *      and the shadow over-charges (looked like mass lost updates).
 *
 * usage: drv_diff_ops -S seed [-T threads] [-n txns/thread]
 * exit: 0 clean; 2 invariant violation (details on stdout).
 */
#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "expli_tm_api/tm_api.hpp"

extern __thread int tm_nested_call_counter;
extern __thread sigjmp_buf tm_jmpbuf;

// ── shared TM state (all inside the TM region) ───────────────────
static constexpr int NC = 64;    // int32 counters
static constexpr int NP = 64;    // int64 slots (8-byte paths)
static constexpr int NB = 16;    // uint8 slots (byte-merge paths)
static constexpr int NSLOT = 16; // transactional pointer slots (ptr path)
static constexpr int NTGT = 8;   // tm_malloc'd deref targets
static int *CNT = nullptr;
static uint64_t *SLOT = nullptr;
static unsigned char *BYTE = nullptr;
static int **PSLOT = nullptr; // NSLOT pointer words, all in TM region
static int *TGT[NTGT];        // deref targets (tm_malloc'd, TM region)
static constexpr long long CNT_INIT = 1000;
static constexpr long long CNT_LO = -1000000, CNT_HI = 1000000;
static constexpr uint64_t SLOT_INIT = 1ull << 40;
static constexpr int BYTE_INIT = 100, BYTE_LO = 40, BYTE_HI = 220;
static constexpr long long TGT_INIT = 500;
// Scratch counters (last two of CNT) receive scratch writes so that
// "read-only-looking" CHECK txns actually validate; never range-checked.
static constexpr int C_SCRATCH = 62, C_SCRATCH2 = 63;

// Shadow deltas (plain atomics — NOT transactional; flushed post-commit)
static std::atomic<long long> sh_c[NC], sh_p[NP];
static std::atomic<int> sh_b[NB];
static std::atomic<long long> sh_t[NTGT];

// Stats
static std::atomic<uint64_t> g_commits{0}, g_aborts{0};
static std::atomic<uint64_t> g_v_double_read{0}, g_v_range{0}, g_v_readown{0};
static std::atomic<uint64_t> g_v_ptr{0};
static int T_n = 4;

// Verdicts deferred to commit-time (thread-local until tm_end returns)
static __thread bool t_bad_dr = false, t_bad_rg = false, t_bad_pt = false;

// ── shadow snapshot for tolerance-bounded range checks ──────────
struct Snap {
	long long c[NC];
	uint64_t p[NP];
	int b[NB];
	long long t[NTGT];
};
static void snap_take(Snap &s)
{
	for (int i = 0; i < NC; i++)
		s.c[i] = CNT_INIT + sh_c[i].load();
	for (int i = 0; i < NP; i++)
		s.p[i] = SLOT_INIT + (uint64_t)(long long)sh_p[i].load();
	for (int i = 0; i < NB; i++)
		s.b[i] = BYTE_INIT + sh_b[i].load();
	for (int i = 0; i < NTGT; i++)
		s.t[i] = TGT_INIT + sh_t[i].load();
}
// Generous: tolerate the bounded per-object drift from <=T_n concurrent
// commits (each <=2 scratch adds of a byte-slot + a transfer) so only
// *wild* (uncommitted/garbage/stale-by-magnitude) reads trip it.
static long long tol() { return (long long)T_n * 32 * 4; }
static long long tol_b() { return (long long)T_n * 3 * 8 + 16; }

struct Delta {
	int kind; // 0=CNT 1=SLOT 2=BYTE 3=TGT(deref through ptr slot)
	int idx;
	long long d;
};
// Commit journal (DIFF_OPS_LOG=1): independent record of every commit's
// deltas, replayed at the end against both the shadow and the data.
static bool g_log_on = false;
static std::vector<Delta> g_journal;
static std::mutex g_jmu;
struct TxBuf {
	Delta e[8];
	int n = 0;
	void clear() { n = 0; }
	void add(int k, int i, long long d)
	{
		if (n < 8)
			e[n++] = {k, i, d};
	}
	void flush() const
	{
		for (int i = 0; i < n; i++) {
			switch (e[i].kind) {
			case 0:
				sh_c[e[i].idx].fetch_add(e[i].d);
				break;
			case 1:
				sh_p[e[i].idx].fetch_add(e[i].d);
				break;
			case 3:
				sh_t[e[i].idx].fetch_add(e[i].d);
				break;
			default:
				sh_b[e[i].idx].fetch_add((int)e[i].d);
				break;
			}
		}
	}
};

// Runs the transaction body; on return the delta list in `buf` is ready
// but NOT yet applied — the caller flushes only after tm_end() commits.
static __thread int s_cbase = 0, s_nc = NC, s_pbase = 0, s_np = NP, s_bbase = 0,
                    s_nb = NB;
// Op-class mask (bit0 ADD, bit1 TRANSFER, bit2 CHECK, bit3 READOWN,
// bit4 SLOTDR, bit5 PTRCHURN, bit6 SMALLBATCH, bit7 Wave-D alloc check)
// for bisecting which operation class loses committed writes.
static int g_opmask = 0xff;
// Which tm_malloc'd target does p denote? -1 = illegal (never a legal target
// ⇒ pointer-metadata corruption). Targets are 4-byte-aligned inside the TM
// region; only exact matches count (backends may legitimately copy blocks,
// so we compare against the bookkeeping pointers we handed out).
static int tgt_idx(const int *p)
{
	for (int i = 0; i < NTGT; i++)
		if (TGT[i] == p)
			return i;
	return -1;
}
static void run_op(std::mt19937 &rng, const Snap &sn, TxBuf &buf)
{
	int roll = (int)(rng() % 100);
	if (roll < 40) { // ADD: counters + slot + byte
		if (!(g_opmask & 1))
			return;
		for (int r = 0; r < 3; r++) {
			int i = s_cbase + (int)(rng() % s_nc);
			long long k = (long long)(rng() % 17) - 8;
			if (!k)
				k = 1;
			long long v = expli::TM<int>::read_at(CNT + i);
			if (v + k >= CNT_LO && v + k <= CNT_HI) {
				expli::TM<int>::write_at(CNT + i, (int)(v + k));
				buf.add(0, i, k);
			}
		}
		{
			int i = s_pbase + (int)(rng() % s_np);
			long long k = (long long)(rng() % 17) - 8;
			if (!k)
				k = 1;
			uint64_t v = expli::TM<uint64_t>::read_at(SLOT + i);
			expli::TM<uint64_t>::write_at(SLOT + i, (uint64_t)((long long)v + k));
			buf.add(1, i, k);
		}
		{
			int i = s_bbase + (int)(rng() % s_nb);
			int k = (int)(rng() % 7) - 3;
			if (!k)
				k = 1;
			int v = expli::TM<unsigned char>::read_at(BYTE + i);
			if (v + k >= BYTE_LO && v + k <= BYTE_HI) {
				expli::TM<unsigned char>::write_at(BYTE + i, (unsigned char)(v + k));
				buf.add(2, i, k);
			}
		}
	} else if (roll < 60) { // TRANSFER between counters (scratch excluded)
		if (!(g_opmask & 2))
			return;
		int i = s_cbase + (int)(rng() % s_nc);
		int j = s_cbase + (int)(rng() % s_nc);
		if (i == j)
			j = s_cbase + ((j - s_cbase + 1) % s_nc);
		long long k = (long long)(rng() % 8) + 1;
		long long bi = expli::TM<int>::read_at(CNT + i);
		long long bj = expli::TM<int>::read_at(CNT + j);
		if (bi - k >= CNT_LO && bj + k <= CNT_HI) {
			expli::TM<int>::write_at(CNT + i, (int)(bi - k));
			expli::TM<int>::write_at(CNT + j, (int)(bj + k));
			buf.add(0, i, -k);
			buf.add(0, j, +k);
		}
	} else if (roll < 80) { // CHECK: repeatable read + bounded range (read-write)
		if (!(g_opmask & 4))
			return;
		// Scratch write forces validation even on ROCO backends (scoped to
		// the thread's region slice so -R mode stays fully disjoint).
		{
			long long v = expli::TM<int>::read_at(CNT + s_cbase);
			expli::TM<int>::write_at(CNT + s_cbase, (int)(v + 1));
			buf.add(0, s_cbase, 1);
		}
		for (int r = 0; r < 3; r++) {
			int i = s_cbase + (int)(rng() % s_nc);
			long long v1 = expli::TM<int>::read_at(CNT + i);
			volatile long long pad = expli::TM<int>::read_at(CNT + s_cbase +
			                                                 (int)(rng() % s_nc));
			(void)pad;
			long long v2 = expli::TM<int>::read_at(CNT + i);
			if (v1 != v2)
				t_bad_dr = true;
			if (v1 - sn.c[i] > tol() || sn.c[i] - v1 > tol())
				t_bad_rg = true;
		}
		int i = s_bbase + (int)(rng() % s_nb);
		int v1 = expli::TM<unsigned char>::read_at(BYTE + i);
		volatile int pad = expli::TM<int>::read_at(CNT + s_cbase + (int)(rng() % s_nc));
		(void)pad;
		int v2 = expli::TM<unsigned char>::read_at(BYTE + i);
		if (v1 != v2)
			t_bad_dr = true;
		if (v1 - sn.b[i] > tol_b() || sn.b[i] - v1 > tol_b())
			t_bad_rg = true;
	} else if (roll < 92) { // READOWN on counter + byte path
		if (!(g_opmask & 8))
			return;
		int i = s_cbase + (int)(rng() % s_nc);
		long long k = (long long)(rng() % 8) + 1;
		long long v = expli::TM<int>::read_at(CNT + i);
		if (v + k <= CNT_HI) {
			expli::TM<int>::write_at(CNT + i, (int)(v + k));
			buf.add(0, i, k);
			if (expli::TM<int>::read_at(CNT + i) != v + k)
				g_v_readown.fetch_add(1);
		}
		int b = s_bbase + (int)(rng() % s_nb);
		int bk = (int)(rng() % 3) + 1;
		int bv = expli::TM<unsigned char>::read_at(BYTE + b);
		if (bv + bk <= BYTE_HI) {
			expli::TM<unsigned char>::write_at(BYTE + b, (unsigned char)(bv + bk));
			buf.add(2, b, bk);
			if (expli::TM<unsigned char>::read_at(BYTE + b) != bv + bk)
				g_v_readown.fetch_add(1);
		}
	} else if (
	    roll <
	    95) { // 8-byte slot double-read (pointer-width merge paths) + scratch write
		if (!(g_opmask & 16))
			return;
		long long sv = expli::TM<int>::read_at(CNT + C_SCRATCH2);
		expli::TM<int>::write_at(CNT + C_SCRATCH2, (int)(sv + 1));
		buf.add(0, C_SCRATCH2, 1);
		int i = s_pbase + (int)(rng() % s_np);
		uint64_t v1 = expli::TM<uint64_t>::read_at(SLOT + i);
		volatile long long pad = expli::TM<int>::read_at(CNT + s_cbase +
		                                                 (int)(rng() % s_nc));
		(void)pad;
		uint64_t v2 = expli::TM<uint64_t>::read_at(SLOT + i);
		if (v1 != v2)
			t_bad_dr = true;
		long long d = (long long)(v1 - sn.p[i]);
		if (d > tol() || d < -tol())
			t_bad_rg = true;
	} else if (roll < 98) { // PTRCHURN: transactional pointer slots deref'd to
		// tm_malloc'd targets. The address written is derived from a
		// TRANSACTIONAL POINTER READ, so a backend that confuses pointer
		// metadata with value data, or that mis-versions indirect
		// addresses (the TM<TM<int>*> shape from test_tx), shows up here.
		if (!(g_opmask & 32))
			return;
		int j = (int)(rng() % NSLOT);
		int *p = (int *)tm_read_ptr((void **)(PSLOT + j));
		int k = tgt_idx(p);
		if (k < 0) {
			t_bad_pt = true; // garbage pointer observed (charged on commit)
		} else {
			long long m = (long long)(rng() % 9) - 4;
			if (!m)
				m = 1;
			long long v = expli::TM<int>::read_at(p);
			expli::TM<int>::write_at(p, (int)(v + m));
			buf.add(3, k, m);
			if (expli::TM<int>::read_at(p) != v + m)
				g_v_readown.fetch_add(1);
			if ((int *)tm_read_ptr((void **)(PSLOT + j)) != p)
				t_bad_dr = true;
		}
		if ((int)(rng() % 4) == 0) { // repoint: slot j := slot j2's pointer
			int j2 = (int)(rng() % NSLOT);
			int *q = (int *)tm_read_ptr((void **)(PSLOT + j2));
			if (tgt_idx(q) < 0)
				t_bad_pt = true;
			else
				tm_write_ptr((void **)(PSLOT + j), (void *)q);
		}
		// deref a random target directly (sanity on the versioned block)
		int k2 = (int)(rng() % NTGT);
		long long dv = expli::TM<int>::read_at(TGT[k2]);
		if (dv - sn.t[k2] > tol() || sn.t[k2] - dv > tol())
			t_bad_rg = true;
	} else { // SMALLBATCH: RMW *all* separately-malloc'd 4-byte blocks in
		// ONE transaction, then read every one back — the exact shape of
		// test_tx's TM<TM<int>*> case that CSMV fails (many tiny adjacent
		// TM allocations written in a single tx: per-word version/guard
		// aliasing between neighbours is the classic failure here).
		if (!(g_opmask & 64))
			return;
		long long ms[NTGT];
		long long vs[NTGT];
		for (int k = 0; k < NTGT; k++) {
			ms[k] = (long long)(rng() % 7) - 3;
			if (!ms[k])
				ms[k] = 1;
			vs[k] = expli::TM<int>::read_at(TGT[k]);
			expli::TM<int>::write_at(TGT[k], (int)(vs[k] + ms[k]));
		}
		for (int k = 0; k < NTGT; k++) {
			if (expli::TM<int>::read_at(TGT[k]) != vs[k] + ms[k])
				g_v_readown.fetch_add(1);
			buf.add(3, k, ms[k]);
		}
	}
}

// Allocator lifecycle self-check (Wave D), mirroring test_tx's tm_realloc
// case: in-tx malloc, plain+transactional stores, grow, tail write/read,
// shrink, prefix check, free. Returns the number of failed checks (0 = ok).
static int allocator_selfcheck(void)
{
	tm_nested_call_counter = 1;
	tm_set_jmpbuf(&tm_jmpbuf);
	if (sigsetjmp(tm_jmpbuf, 0))
		return 0; // aborted → speculative block
	              // rolled back; count nothing
	tm_begin();
	int fails = 0;
	const int n_old = 16, n_new = 64;
	int *p = (int *)expli::TM<int>::malloc(n_old * sizeof(int));
	if (!p) {
		tm_end();
		return 1;
	}
	for (int i = 0; i < n_old; i++) {
		p[i] = i + 1;
		expli::TM<int>::write_at(&p[i], i + 1);
	}
	int *q = (int *)expli::TM<int>::realloc(p, n_new * sizeof(int));
	if (!q) {
		tm_end();
		return 1;
	}
	for (int i = 0; i < n_old; i++)
		if (expli::TM<int>::read_at(&q[i]) != i + 1)
			fails++;
	for (int i = n_old; i < n_new; i++)
		expli::TM<int>::write_at(&q[i], i);
	if (expli::TM<int>::read_at(&q[n_new - 1]) != n_new - 1)
		fails++;
	if (expli::TM<int>::read_at(&q[n_old]) != n_old)
		fails++;
	int *r = (int *)expli::TM<int>::realloc(q, 8 * sizeof(int));
	if (!r) {
		tm_end();
		return fails + 1;
	}
	for (int i = 0; i < 8; i++)
		if (expli::TM<int>::read_at(&r[i]) != i + 1)
			fails++;
	expli::TM<int>::free(r);
	tm_end();
	return fails;
}

// One logical transaction (retried until it commits).
static void tx_once(std::mt19937 &rng)
{
	TxBuf buf;
	while (true) {
		int r = sigsetjmp(tm_jmpbuf, 0); // abort retries re-enter HERE...
		if (r)                           // ...so EVERY per-attempt reset and
			g_aborts.fetch_add(1, std::memory_order_relaxed);
		buf.clear(); // counter must go AFTER the sigsetjmp return point:
		t_bad_dr = t_bad_rg = t_bad_pt = false;
		// an abort longjmps back to the line above and SKIPS anything
		// placed before it in this body — clearing the delta buffer here
		// (rather than before sigsetjmp) is what stops retries from
		// double-charging abandoned attempts into the shadow model.
		Snap sn;
		snap_take(sn);
		tm_nested_call_counter = 1;
		tm_set_jmpbuf(&tm_jmpbuf);
		tm_begin();
		run_op(rng, sn, buf);
		tm_end(); // abort -> siglongjmp back to sigsetjmp above
		break;
	}
	// Committed (tm_end returned): NOW charge the shadow + deferred verdicts.
	if (g_log_on) {
		std::lock_guard<std::mutex> lk(g_jmu);
		for (int i = 0; i < buf.n; i++)
			g_journal.push_back(buf.e[i]);
	}
	buf.flush();
	g_commits.fetch_add(1, std::memory_order_relaxed);
	if (t_bad_dr)
		g_v_double_read.fetch_add(1);
	if (t_bad_rg)
		g_v_range.fetch_add(1);
	if (t_bad_pt)
		g_v_ptr.fetch_add(1);
}

int main(int argc, char **argv)
{
	unsigned seed = 1;
	int NT = 4;
	long nops = 1500;
	bool region_mode = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-S") && i + 1 < argc)
			seed = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-T") && i + 1 < argc)
			NT = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			nops = atol(argv[++i]);
		else if (!strcmp(argv[i], "-R"))
			region_mode = true;
		else if (!strcmp(argv[i], "-M") && i + 1 < argc)
			g_opmask = atoi(argv[++i]);
	}
	if (region_mode && (NC % NT || NP % NT || NB % NT)) {
		printf("-R needs NT dividing %d/%d/%d\n", NC, NP, NB);
		return 1;
	}
	T_n = NT;
	g_log_on = getenv("DIFF_OPS_LOG") != nullptr;
	expli::TM<int>::init();
	CNT = (int *)expli::TM<int>::malloc(NC * sizeof(int));
	SLOT = (uint64_t *)expli::TM<int>::malloc(NP * sizeof(uint64_t));
	BYTE = (unsigned char *)expli::TM<int>::malloc(NB * sizeof(unsigned char));
	PSLOT = (int **)expli::TM<int>::malloc(NSLOT * sizeof(int *));
	if (!CNT || !SLOT || !BYTE || !PSLOT) {
		printf("region malloc failed\n");
		return 1;
	}
	for (int i = 0; i < NTGT; i++) {
		TGT[i] = (int *)expli::TM<int>::malloc(sizeof(int));
		if (!TGT[i]) {
			printf("region malloc failed\n");
			return 1;
		}
		*TGT[i] = (int)TGT_INIT;
	}
	for (int i = 0; i < NC; i++)
		CNT[i] = (int)CNT_INIT;
	for (int i = 0; i < NP; i++)
		SLOT[i] = SLOT_INIT;
	for (int i = 0; i < NB; i++)
		BYTE[i] = (unsigned char)BYTE_INIT;
	for (int j = 0; j < NSLOT; j++)
		PSLOT[j] = TGT[j % NTGT];
	if (getenv("DIFF_OPS_DIAG")) {
		printf("DIAG layout CNT=%p..%p SLOT=%p..%p BYTE=%p..%p PSLOT=%p..%p\n",
		       (void *)CNT,
		       (void *)(CNT + NC),
		       (void *)SLOT,
		       (void *)(SLOT + NP),
		       (void *)BYTE,
		       (void *)(BYTE + NB),
		       (void *)PSLOT,
		       (void *)(PSLOT + NSLOT));
		printf("DIAG TGT=");
		for (int i = 0; i < NTGT; i++)
			printf("%p ", (void *)TGT[i]);
		printf("\n");
		for (int i = 0; i < NC; i++)
			if (CNT[i] != (int)CNT_INIT)
				printf("DIAG CNT[%d] pre-corrupted to %d\n", i, CNT[i]);
		for (int i = 0; i < NP; i++)
			if (SLOT[i] != SLOT_INIT)
				printf("DIAG SLOT[%d] pre-corrupted to %llu\n",
				       i,
				       (unsigned long long)SLOT[i]);
		for (int i = 0; i < NB; i++)
			if (BYTE[i] != BYTE_INIT)
				printf("DIAG BYTE[%d] pre-corrupted to %d\n", i, BYTE[i]);
	}

	auto worker = [&](int tid, long ops) {
		expli::TM<int>::thread_init();
		if (region_mode) {
			int s = tid % NT;
			s_cbase = s * (NC / NT);
			s_nc = NC / NT;
			s_pbase = s * (NP / NT);
			s_np = NP / NT;
			s_bbase = s * (NB / NT);
			s_nb = NB / NT;
		}
		if (getenv("DIFF_OPS_DIAG") && tid < 2)
			printf("DIAG worker tid=%d tm_read_i4=%p tm_write_i4=%p tm_begin=%p\n",
			       tid,
			       (void *)tm_read_i4,
			       (void *)tm_write_i4,
			       (void *)tm_begin);
		std::mt19937 rng(seed * 7919u + (unsigned)tid);
		for (long o = 0; o < ops; o++)
			tx_once(rng);
		expli::TM<int>::thread_exit();
	};

	// Wave A: heavy concurrent phase
	std::vector<std::thread> ts;
	for (int t = 0; t < NT; t++)
		ts.emplace_back(worker, t, nops);
	for (auto &t : ts)
		t.join();

	auto quiesce_check = [&]() -> int {
		int v = 0;
		for (int i = 0; i < NC; i++)
			if ((long long)CNT[i] != CNT_INIT + sh_c[i].load())
				v++;
		for (int i = 0; i < NP; i++)
			if (SLOT[i] != SLOT_INIT + (uint64_t)(long long)sh_p[i].load())
				v++;
		for (int i = 0; i < NB; i++)
			if ((int)BYTE[i] != BYTE_INIT + sh_b[i].load())
				v++;
		return v;
	};
	if (getenv("DIFF_OPS_CHECK_EARLY"))
		printf("EARLY(quiesce after wave A) viol=%d commits=%llu aborts=%llu\n",
		       quiesce_check(),
		       (unsigned long long)g_commits.load(),
		       (unsigned long long)g_aborts.load());

	// Wave B: short-lived threads (TLS lifecycle / init-exit churn)
	for (int t = 0; t < 48; t++) {
		std::thread w(worker, 1000 + t, 3);
		w.join();
	}

	// Wave C: main thread after all workers exited (state-reuse paths)
	expli::TM<int>::thread_init();
	std::mt19937 rng(seed * 104729u);
	for (int o = 0; o < 200; o++)
		tx_once(rng);

	// Wave D: single-threaded allocator lifecycle self-check — the exact
	// shape of test_tx's tm_realloc case, which is the one test_tx actually
	// fails on CSMV ("tail writable"; the other FAIL labels are the shared
	// failure counter bleeding). Run HERE, after the concurrent waves,
	// because slab/region state differs once it has churned. Blocks are
	// private + freed, so this is not part of the additive model.
	int alloc_viol = 0;
	if (g_opmask & 128) {
		for (int rep = 0; rep < 64; rep++) {
			int before = alloc_viol;
			alloc_viol += allocator_selfcheck();
			if (alloc_viol > before)
				printf("VIOLATION alloc rep=%d\n", rep);
		}
	}
	expli::TM<int>::thread_exit();

	// ── quiescence check: final == init + Σ committed deltas, per object ──
	int viol = 0;
	for (int i = 0; i < NC; i++) {
		long long want = CNT_INIT + sh_c[i].load();
		if ((long long)CNT[i] != want) {
			if (viol < 10)
				printf("VIOLATION CNT[%d]: final=%lld shadow=%lld\n",
				       i,
				       (long long)CNT[i],
				       want);
			viol++;
		}
	}
	for (int i = 0; i < NP; i++) {
		uint64_t want = SLOT_INIT + (uint64_t)(long long)sh_p[i].load();
		if (SLOT[i] != want) {
			if (viol < 10)
				printf("VIOLATION SLOT[%d]: final=%llu shadow=%llu\n",
				       i,
				       (unsigned long long)SLOT[i],
				       (unsigned long long)want);
			viol++;
		}
	}
	for (int i = 0; i < NB; i++) {
		int want = BYTE_INIT + sh_b[i].load();
		if ((int)BYTE[i] != want) {
			if (viol < 10)
				printf("VIOLATION BYTE[%d]: final=%d shadow=%d\n", i, (int)BYTE[i], want);
			viol++;
		}
	}
	for (int i = 0; i < NTGT; i++) {
		long long want = TGT_INIT + sh_t[i].load();
		if ((long long)*TGT[i] != want) {
			if (viol < 10)
				printf("VIOLATION TGT[%d]: final=%lld shadow=%lld\n",
				       i,
				       (long long)*TGT[i],
				       want);
			viol++;
		}
	}
	// Pointer-slot legality: repoints only ever install legal targets, so
	// every final PSLOT must denote one of them.
	for (int j = 0; j < NSLOT; j++) {
		if (tgt_idx(PSLOT[j]) < 0) {
			if (viol < 10)
				printf("VIOLATION PSLOT[%d]: final=%p not a legal target\n",
				       j,
				       (void *)PSLOT[j]);
			viol++;
		}
	}
	uint64_t vr = g_v_double_read.load(), vg = g_v_range.load(), vo = g_v_readown.load();
	if (getenv("DIFF_OPS_DUMP")) {
		long long want_sum = 0, got_sum = 0;
		for (int i = 0; i < NC; i++) {
			want_sum += CNT_INIT + sh_c[i].load();
			got_sum += CNT[i];
		}
		printf("SUM CNT want=%lld got=%lld diff=%+lld\n",
		       want_sum,
		       got_sum,
		       got_sum - want_sum);
		long long psum_w = 0, psum_g = 0;
		for (int i = 0; i < NP; i++) {
			psum_w += (long long)SLOT_INIT + sh_p[i].load();
			psum_g += (long long)SLOT[i];
		}
		printf("SUM SLOT want=%lld got=%lld diff=%+lld\n",
		       psum_w,
		       psum_g,
		       psum_g - psum_w);
		long long bsum_w = 0, bsum_g = 0;
		for (int i = 0; i < NB; i++) {
			bsum_w += BYTE_INIT + sh_b[i].load();
			bsum_g += BYTE[i];
		}
		printf("SUM BYTE want=%lld got=%lld diff=%+lld\n",
		       bsum_w,
		       bsum_g,
		       bsum_g - bsum_w);
		for (int i = 0; i < NC; i++)
			if ((long long)CNT[i] != CNT_INIT + sh_c[i].load())
				printf("DUMP CNT[%d] final=%lld shadow=%lld diff=%+lld\n",
				       i,
				       (long long)CNT[i],
				       CNT_INIT + sh_c[i].load(),
				       (long long)CNT[i] - CNT_INIT - sh_c[i].load());
		for (int i = 0; i < NP; i++) {
			uint64_t want = SLOT_INIT + (uint64_t)(long long)sh_p[i].load();
			if (SLOT[i] != want)
				printf("DUMP SLOT[%d] diff=%+lld\n", i, (long long)(SLOT[i] - want));
		}
		for (int i = 0; i < NB; i++)
			if ((int)BYTE[i] != BYTE_INIT + sh_b[i].load())
				printf("DUMP BYTE[%d] diff=%+d\n",
				       i,
				       (int)BYTE[i] - BYTE_INIT - sh_b[i].load());
		// Journal replay: apply every RECORDED commit delta (independent
		// of the shadow's fetch_add path) and compare vs final data.
		//   replay==shadow!=data  -> writes did not land  (backend)
		//   replay!=shadow        -> harness accounting bug
		//   replay==data!=shadow  -> shadow charged wrong deltas
		if (g_log_on) {
			long long rc[NC];
			long long rp[NP];
			int rb[NB];
			long long rt[NTGT];
			for (int i = 0; i < NC; i++)
				rc[i] = CNT_INIT;
			for (int i = 0; i < NP; i++)
				rp[i] = (long long)SLOT_INIT;
			for (int i = 0; i < NB; i++)
				rb[i] = BYTE_INIT;
			for (int i = 0; i < NTGT; i++)
				rt[i] = TGT_INIT;
			for (auto &d : g_journal) {
				if (d.kind == 0)
					rc[d.idx] += d.d;
				else if (d.kind == 1)
					rp[d.idx] += d.d;
				else if (d.kind == 3)
					rt[d.idx] += d.d;
				else
					rb[d.idx] += (int)d.d;
			}
			int rd = 0, rs = 0;
			for (int i = 0; i < NC; i++) {
				if (rc[i] != (long long)CNT[i])
					rd++;
				if (rc[i] != CNT_INIT + sh_c[i].load())
					rs++;
			}
			for (int i = 0; i < NP; i++) {
				if (rp[i] != (long long)SLOT[i])
					rd++;
				if (rp[i] != (long long)SLOT_INIT + sh_p[i].load())
					rs++;
			}
			for (int i = 0; i < NB; i++) {
				if (rb[i] != (int)BYTE[i])
					rd++;
				if (rb[i] != BYTE_INIT + sh_b[i].load())
					rs++;
			}
			for (int i = 0; i < NTGT; i++) {
				if (rt[i] != (long long)*TGT[i])
					rd++;
				if (rt[i] != TGT_INIT + sh_t[i].load())
					rs++;
			}
			printf("JOURNAL entries=%zu replay_vs_data_viol=%d "
			       "replay_vs_shadow_viol=%d\n",
			       g_journal.size(),
			       rd,
			       rs);
		}
	}
	printf("RESULT seed=%u T=%d commits=%llu aborts=%llu v_double_read=%llu "
	       "v_range=%llu v_readown=%llu v_ptr=%llu quiesce_viol=%d alloc_viol=%d\n",
	       seed,
	       NT,
	       (unsigned long long)g_commits.load(),
	       (unsigned long long)g_aborts.load(),
	       (unsigned long long)vr,
	       (unsigned long long)vg,
	       (unsigned long long)vo,
	       (unsigned long long)g_v_ptr.load(),
	       viol,
	       alloc_viol);
	expli::TM<int>::exit();
	return (viol || alloc_viol || vr || vg || vo || g_v_ptr.load()) ? 2 : 0;
}
