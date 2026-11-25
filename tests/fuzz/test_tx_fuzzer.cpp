// Fuzz target: TM transaction operations
// libFuzzer test exercising TM begin/end/read/write/malloc/free sequences.
//
// Harness invariants (violating either re-introduces the fuzz-smoke OOM,
// exit 71, found on CI run 39127621218):
//   1. Backend lifecycle (tm_init/tm_init_thread) runs exactly once.
//   2. expli::TM<T> objects live in the TM region, which has no per-object
//      free: they must be created once and must outlive any single input.
//   3. Every input runs its op stream inside one outer transaction via
//      TM::transaction() (sigsetjmp frame in that frame), so read/write/
//      peek never observe an inactive tx, and the per-tx write set +
//      deferred-free list are flushed at every commit.

#include "expli_tm_api/tm_api.hpp"
#include <cstdint>
#include <cstring>

// The op stream is capped at max_ops, so this can never overflow.
#define FUZZ_MAX_ALLOCS 64

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
	if (Size < 2)
		return 0;

	static const bool inited = [] {
		expli::TM<int>::init();
		expli::TM<int>::thread_init();
		return true;
	}();
	(void)inited;

	// Created once: TM<T> values are tm_malloc'd from the fixed TM region
	// and are never released per-object.
	static expli::TM<int> tms[4];
	static const bool poked = [] {
		for (int i = 0; i < 4; i++)
			tms[i].poke(0);
		return true;
	}();
	(void)poked;

	// Operation codes:
	// 0 = TM_begin, 1 = TM_end, 2 = TM_read, 3 = TM_write
	// 4 = TM_malloc, 5 = TM_free, 6 = TM_poke, 7 = TM_peek
	const int max_ops = 20;

	// Per-attempt TM_malloc() results, freed inside the transaction at the
	// end of the op stream.  Reset at every (re-)attempt: an abort rolls
	// speculative allocations back with the transaction, so those pointers
	// must not be referenced again.
	void *alloced[FUZZ_MAX_ALLOCS];

	expli::TM<int>::transaction([&] {
		int n_alloc = 0;
		int ops_count = 0;
		uint8_t op = Data[0];

		while (ops_count < max_ops && ops_count < (int)Size) {
			ops_count++;

			switch (op % 8) {
			case 0: // TM_begin (nested)
				expli::TM<int>::begin();
				break;
			// TM_end: only nested levels are closed; the outer
			// transaction frame owns the commit.
			case 1:
				if (tm_nested_call_counter > 1)
					expli::TM<int>::end();
				break;
			case 2: { // TM_read
				if (Size > ops_count) {
					uint8_t obj = Data[ops_count];
					if (obj < 4) {
						int val = tms[obj].read();
						(void)val; // Prevent unused variable
					}
				}
				break;
			}
			case 3: { // TM_write
				if (Size > ops_count + 1) {
					uint8_t obj = Data[ops_count + 1];
					uint8_t val = Data[ops_count + 2];
					if (obj < 4) {
						tms[obj].write(val);
					}
					ops_count++; // skip extra bytes
				}
				break;
			}
			case 4: { // TM_malloc
				if (Size > ops_count + 1) {
					size_t sz = 16; // default small size
					if (ops_count + 2 < Size) {
						sz = Data[ops_count + 1] % 100 + 1;
					}
					int *p = (int *)expli::TM<int>::malloc(sz);
					if (p && n_alloc < FUZZ_MAX_ALLOCS)
						alloced[n_alloc++] = p;
					ops_count++; // skip size parameter
				}
				break;
			}
			case 5: { // TM_free
				if (Size > ops_count + 1) {
					uint8_t obj = Data[ops_count + 1];
					if (obj < 4) {
						// We can't easily free tms[obj] since it's static,
						// but we test the path is reachable
					}
					ops_count++; // skip obj parameter
				}
				break;
			}
			case 6: { // TM_poke
				if (Size > ops_count + 1) {
					uint8_t obj = Data[ops_count + 1];
					uint8_t val = Data[ops_count + 2];
					if (obj < 4) {
						tms[obj].poke(val);
					}
					ops_count++; // skip extra bytes
				}
				break;
			}
			case 7: { // TM_peek
				if (Size > ops_count + 1) {
					uint8_t obj = Data[ops_count + 1];
					if (obj < 4) {
						(void)tms[obj].peek();
					}
					ops_count++; // skip extra bytes
				}
				break;
			}
			}

			op = Data[ops_count]; // next operation code
		}

		// Release this attempt's allocations inside the tx: real_tm_free()
		// untracks the speculative allocation and defers the region block
		// to the commit, so the region stays reused, not consumed.
		for (int i = 0; i < n_alloc; i++)
			expli::TM<int>::free(alloced[i]);
	});

	return 0;
}
