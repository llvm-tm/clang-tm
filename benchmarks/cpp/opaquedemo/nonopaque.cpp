// nonopaque.cpp — the book's Chapter 2 worked example: opacity, made real.
//
// Build once per backend (the demo is one source, two semantics):
//
//   make bin/opaquedemo BACKEND=NOREC         -> exits 0, no phantom state
//   make bin/opaquedemo BACKEND=EAGER_BROKEN  -> SIGSEGV (exit 139), always
//
// The writer transaction publishes an item into a shared inventory and
// then ABORTS: a spoiler thread bumps `seed`, so the transaction's
// read-set fails commit validation on either backend.  The reader is
// deliberately NOT in a transaction — it consumes the inventory the way
// privatized code does: latch the count, dereference the slots.
//
// NOrec sandboxes every speculative write, so the reader can never
// observe a count of 1 that is not backed by a committed item; after the
// abort it latches count=0 and scans nothing.  EagerBroken publishes the
// writes immediately: the reader latches count=1 mid-flight, the abort
// then UNDOES the inventory slot (back to NULL) while the latched count
// still says "one item", and the scan dereferences NULL — a state no
// serial execution of this program can produce.  That is opacity,
// violated, and it segfaults.
//
// The interleaving is forced by three barriers, so the outcome is
// deterministic on every run, not a race that "sometimes" crashes.

#include "expli_tm_api/tm_api.hpp"
#include "expli_tm_api/thread_barrier.hpp"
#include <cstdint>
#include <cstdio>
#include <thread>

struct Item {
	int32_t id;
	int32_t payload;
};

// Shared TM state. Plain statics; the barrier waits order the raw reads.
static void *g_inventory[4];
static int32_t g_count = 0;
static int32_t g_seed = 1;

static expli::Barrier b_publish(3); // writer, reader, spoiler (main)
static expli::Barrier b_peek(2);    // writer, reader: latch before undo
static expli::Barrier b_spoiled(2); // writer, spoiler
static expli::Barrier b_aborted(2); // writer, reader

static void writer()
{
	tm_init_thread();
	// Arm the retry jump point in THIS frame, which stays alive for the
	// whole transaction (the frame-lifetime rule of Ch. 13).
	int rc = sigsetjmp(tm_jmpbuf, 0);
	if (rc == 0) {
		tm_begin();
		Item *it = (Item *)tm_malloc(sizeof(Item));
		tm_write_i4((uint32_t *)&it->id, 7);
		tm_write_i4((uint32_t *)&it->payload, 700);
		tm_read_i4((uint32_t *)&g_seed); // read-set: seed == 1
		tm_write_ptr(&g_inventory[0], it);
		tm_write_i4((uint32_t *)&g_count, 1);
		b_publish.wait();  // now every other thread can look
		b_peek.wait();     // reader has latched its view: safe to spoil
		b_spoiled.wait();  // seed has been bumped: commit must fail
		tm_end();          // validation fails here -> siglongjmp back
		printf("writer: committed (this should not happen)\n");
	} else {
		printf("writer: attempt ended in validation abort (siglongjmp rc=%d)\n",
		       rc);
	}
	b_aborted.wait();
	tm_exit_thread();
}

static void reader()
{
	tm_init_thread();
	b_publish.wait();

	// Non-transactional consumer: latch the published view.
	int32_t n_latched = *(volatile int32_t *)&g_count;
	void *p0 = *(void *volatile *)&g_inventory[0];
	int32_t id_latched = (n_latched > 0 && p0) ? ((Item *)p0)->id : 0;
	int32_t payload_latched = (n_latched > 0 && p0) ? ((Item *)p0)->payload : 0;
	b_peek.wait(); // let the writer proceed only after our latch is complete
	printf("reader: mid-flight view: count=%d inventory[0]=%p\n",
	       n_latched, p0);
	if (n_latched > 0 && p0) {
		printf("reader: phantom item visible (id=%d payload=%d) "
		       "— the transaction has NOT committed!\n",
		       id_latched, payload_latched);
	}

	b_aborted.wait();
	printf("reader: transaction has aborted; final count=%d\n",
	       *(volatile int32_t *)&g_count);

	// The bug the latched view enables: the consumer uses the count it
	// read mid-flight, after the abort has undone the inventory slot.
	for (int32_t i = 0; i < n_latched; ++i) {
		Item *p = (Item *)g_inventory[i];
		printf("reader: item[%d] = {id=%d, payload=%d}\n", i, p->id,
		       p->payload); // EAGER_BROKEN: p == NULL -> SIGSEGV
	}
	if (n_latched == 0)
		printf("reader: consistent view preserved; nothing to scan\n");
	tm_exit_thread();
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // keep the log intact on SIGSEGV
#if defined(TM_BACKEND_EAGERBROKEN)
	printf("== EAGER_BROKEN (trivially eager, non-opaque by design) ==\n");
#elif defined(TM_BACKEND_NOREC)
	printf("== NOREC (opaque: writes sandboxed until commit) ==\n");
#endif
	tm_init();

	std::thread w(writer);
	std::thread r(reader);

	// Spoiler (main): after the publish barrier, bump `seed` INSIDE a
	// transaction of its own (raw stores would bypass the commit clock
	// and leave NOrec's validation trivially satisfied).  Both backends
	// then re-read the seed at the writer's commit, see the mismatch, and
	// abort the writer's transaction.
	tm_init_thread();
	b_publish.wait();
	int src = sigsetjmp(tm_jmpbuf, 0); // arm in main's own frame
	if (src == 0) {
		tm_begin();
		tm_write_i4((uint32_t *)&g_seed, 2);
		tm_end(); // the spoiler has no conflicts: commits
	}
	b_spoiled.wait();

	w.join();
	r.join();
	tm_exit_thread();
	tm_exit();
	printf("main: survived — no phantom state was ever observed\n");
	return 0;
}
