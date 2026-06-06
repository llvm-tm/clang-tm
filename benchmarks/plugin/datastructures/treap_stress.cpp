// Stress test for TMTreapMap / TMTreapMultiMap
// Compile: clang++ -std=c++20 -O2 treap_stress.cpp -o treap_stress

#include "tm_treap_map.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    std::srand(42);

    // === TMTreapMap tests ===

    // 1. Basic insert and find
    {
        TMTreapMap<int, int> m;
        m[1] = 10;
        m[2] = 20;
        m[3] = 30;

        assert(m.size() == 3);
        assert(m[1] == 10);
        assert(m[2] == 20);
        assert(m[3] == 30);
        assert(m.find(1) != m.end());
        assert(m.find(4) == m.end());

        auto it = m.find(2);
        assert(it->first == 2);
        assert(it->second == 20);

        printf("PASS: basic insert/find\n");
    }

    // 2. Overwrite existing key
    {
        TMTreapMap<int, int> m;
        m[5] = 50;
        m[5] = 100;
        assert(m.size() == 1);
        assert(m[5] == 100);
        printf("PASS: overwrite\n");
    }

    // 3. Erase
    {
        TMTreapMap<int, int> m;
        for (int i = 0; i < 100; i++) m[i] = i * 10;
        assert(m.size() == 100);

        assert(m.erase(50) == 1);
        assert(m.size() == 99);
        assert(m.find(50) == m.end());

        assert(m.erase(999) == 0);
        assert(m.size() == 99);

        printf("PASS: erase\n");
    }

    // 4. Iterator traversal
    {
        TMTreapMap<int, int> m;
        for (int i = 0; i < 100; i++) m[i] = i * 10;

        int count = 0;
        int prev_key = -1;
        for (auto it = m.begin(); it != m.end(); ++it) {
            assert(it->first > prev_key);
            assert(it->second == it->first * 10);
            prev_key = it->first;
            count++;
        }
        assert(count == 100);
        printf("PASS: iterator (sorted order)\n");
    }

    // 5. Clear and empty
    {
        TMTreapMap<int, int> m;
        m[1] = 10;
        m[2] = 20;
        assert(!m.empty());
        m.clear();
        assert(m.empty());
        assert(m.size() == 0);
        assert(m.begin() == m.end());
        printf("PASS: clear/empty\n");
    }

    // 6. Large random insert + find
    {
        TMTreapMap<int, int> m;
        std::vector<int> keys;
        for (int i = 0; i < 10000; i++) {
            int k = std::rand() % 100000;
            m[k] = k * 2;
            keys.push_back(k);
        }

        // Verify all inserted keys are findable
        for (int k : keys) {
            auto it = m.find(k);
            assert(it != m.end());
            assert(it->second == k * 2);
        }

        printf("PASS: large random (%zu entries)\n", m.size());
    }

    // === TMTreapMultiMap tests ===

    // 7. MultiMap insert and lower_bound
    {
        TMTreapMultiMap<int, int> mm;
        mm.insert({2, 20});
        mm.insert({1, 10});
        mm.insert({3, 30});
        mm.insert({2, 21});  // duplicate key

        assert(mm.size() == 4);

        auto it = mm.lower_bound(2);
        assert(it != mm.end());
        assert(it->first == 2);
        // First 2-valued entry
        assert(it->second == 20 || it->second == 21);
        ++it;
        assert(it->first == 2);  // second duplicate
        ++it;
        assert(it->first == 3);

        printf("PASS: multimap insert/lower_bound\n");
    }

    // 8. MultiMap range iteration
    {
        TMTreapMultiMap<int, int> mm;
        mm.insert({1, 100});
        mm.insert({2, 200});
        mm.insert({2, 201});
        mm.insert({2, 202});
        mm.insert({3, 300});

        // Count all elements with key == 2
        auto lo = mm.lower_bound(2);
        auto hi_l = mm.lower_bound(3);
        int count = 0;
        for (auto it = lo; it != hi_l; ++it) {
            assert(it->first == 2);
            count++;
        }
        assert(count == 3);

        printf("PASS: multimap range iteration\n");
    }

    // 9. MultiMap with same key inserted many times
    {
        TMTreapMultiMap<int, int> mm;
        for (int i = 0; i < 1000; i++)
            mm.insert({0, i});

        assert(mm.size() == 1000);

        auto it = mm.lower_bound(0);
        for (int i = 0; i < 1000; i++) {
            assert(it != mm.end());
            assert(it->first == 0);
            ++it;
        }
        assert(it == mm.end());

        printf("PASS: multimap 1000 same keys\n");
    }

    // 10. Clear empty
    {
        TMTreapMultiMap<int, int> mm;
        mm.clear();
        assert(mm.empty());
        mm.insert({1, 10});
        mm.clear();
        assert(mm.empty());
        printf("PASS: multimap clear\n");
    }

    printf("\nAll tests PASSED!\n");
    return 0;
}
