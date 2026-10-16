// Fuzz target: TM data structure operations
// libFuzzer test exercising the flat_map / flat_multimap / flat_set containers
// in explicit_api/cpp/include/tm_map.hpp.

#include "expli_tm_api/tm_api.hpp"
#include "expli_tm_api/tm_map.hpp"
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
	if (Size < 1)
		return 0;

	// Operation codes (op % 8):
	// 0 = insert (map+set), 1 = find/contains, 2 = erase, 3 = clear,
	// 4 = size, 5 = contains, 6 = operator[], 7 = multimap insert.
	uint8_t op = Data[0];
	const int max_ops = 30;
	int ops_count = 1; // skip op code

	expli::flat_map<int, int> m;
	expli::flat_set<int> s;
	expli::flat_multimap<int, int> mm;

	while (ops_count < max_ops && ops_count < (int)Size) {
		const int key = Data[ops_count] % 1000;

		switch (op % 8) {
		case 0: { // insert
			int val = key;
			if (ops_count + 1 < (int)Size)
				val = Data[ops_count + 1] % 1000;
			m.insert(key, val);
			s.insert(key);
			break;
		}
		case 1: // find / contains
			(void)m.find(key);
			(void)s.contains(key);
			break;
		case 2: // erase
			m.erase(key);
			s.erase(key);
			break;
		case 3: // clear
			m.clear();
			s.clear();
			break;
		case 4: // size
			(void)m.size();
			(void)s.size();
			(void)mm.size();
			break;
		case 5: // contains
			(void)s.contains(key);
			break;
		case 6: // operator[]
			(void)m[key];
			break;
		case 7: // multimap insert (allows duplicate keys)
			mm.insert(key, key);
			break;
		}

		ops_count++;
		if (ops_count < (int)Size)
			op = Data[ops_count];
	}

	return 0;
}
