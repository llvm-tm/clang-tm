// Fuzz target: TM transaction operations
// libFuzzer test exercising TM begin/end/read/write/malloc/free sequences.

#include "expli_tm_api/tm_api.hpp"
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
	if (Size < 2)
		return 0;

	// Operation codes:
	// 0 = TM_begin, 1 = TM_end, 2 = TM_read, 3 = TM_write
	// 4 = TM_malloc, 5 = TM_free, 6 = TM_poke, 7 = TM_peek

	uint8_t op = Data[0];

	// Limit operations to avoid runaway fuzzer
	int max_ops = 20;
	int ops_count = 0;

	// TM objects
	expli::TM<int> tms[4];
	for (int i = 0; i < 4; i++) {
		tms[i].poke(0);
	}

	int thread_idx = 0;

	while (ops_count < max_ops && ops_count < Size) {
		ops_count++;

		switch (op % 8) {
		case 0: // TM_begin
			expli::TM<int>::begin();
			break;
		case 1: // TM_end
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
				(void)p;     // Prevent unused variable
				ops_count++; // skip size parameter
			}
			break;
		}
		case 5: { // TM_free
			if (Size > ops_count + 1) {
				uint8_t obj = Data[ops_count + 1];
				if (obj < 4) {
					// We can't easily free tms[obj] since it's on stack,
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

	return 0;
}