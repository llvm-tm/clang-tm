// Test for TMSafeHashSet — validates open-addressing hash set correctness.
#include <cstdio>
#include <cstdlib>
#include "tm_hash_set.hpp"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while(0)

int main()
{
    // Empty set
    {
        TMSafeHashSet<int> s;
        CHECK(s.empty(), "new set is empty");
        CHECK(s.size() == 0, "new set size is 0");
        CHECK(!s.contains(42), "empty set does not contain 42");
    }

    // Single insert / contains
    {
        TMSafeHashSet<int> s;
        CHECK(s.insert(42), "first insert returns true");
        CHECK(s.contains(42), "contains inserted value");
        CHECK(!s.empty(), "set not empty after insert");
        CHECK(s.size() == 1, "set size is 1");
    }

    // Duplicate insert
    {
        TMSafeHashSet<int> s;
        s.insert(42);
        CHECK(!s.insert(42), "duplicate insert returns false");
        CHECK(s.size() == 1, "size unchanged after dup insert");
    }

    // Erase
    {
        TMSafeHashSet<int> s;
        s.insert(1);
        s.insert(2);
        s.insert(3);
        CHECK(s.erase(2), "erase returns true");
        CHECK(s.size() == 2, "size after erase is 2");
        CHECK(!s.contains(2), "erased value not found");
        CHECK(s.contains(1), "other values remain");
        CHECK(s.contains(3), "other values remain");
        CHECK(!s.erase(42), "erase non-existent returns false");
    }

    // Clear
    {
        TMSafeHashSet<int> s;
        s.insert(10);
        s.insert(20);
        s.insert(30);
        s.clear();
        CHECK(s.empty(), "set empty after clear");
        CHECK(!s.contains(10), "cleared set has no values");
    }

    // Many inserts (triggers grow)
    {
        TMSafeHashSet<int> s;
        for (int i = 0; i < 1000; i++) {
            if (!s.insert(i)) {
                char buf[64]; snprintf(buf, sizeof(buf), "insert %d", i);
                CHECK(0, buf);
            }
        }
        CHECK(s.size() == 1000, "size is 1000 after bulk insert");
        for (int i = 0; i < 1000; i++) {
            if (!s.contains(i)) {
                char buf[64]; snprintf(buf, sizeof(buf), "contains %d", i);
                CHECK(0, buf);
            }
        }
        for (int i = 0; i < 1000; i++) {
            if (!s.erase(i)) {
                char buf[64]; snprintf(buf, sizeof(buf), "erase %d", i);
                CHECK(0, buf);
            }
        }
        CHECK(s.empty(), "set empty after erasing all");
    }

    // Copy constructor
    {
        TMSafeHashSet<int> s;
        s.insert(1); s.insert(2); s.insert(3);
        TMSafeHashSet<int> c = s;
        CHECK(c.size() == 3, "copy has same size");
        CHECK(c.contains(1) && c.contains(2) && c.contains(3), "copy has all values");
        c.insert(4);
        CHECK(!s.contains(4), "modifying copy does not affect original");
    }

    // Many keys
    {
        TMSafeHashSet<int> s;
        for (int i = 0; i < 100; i++)
            s.insert(i * 7 + 3);
        for (int i = 0; i < 100; i++) {
            if (!s.contains(i * 7 + 3)) {
                char buf[64]; snprintf(buf, sizeof(buf), "contains key %d", i * 7 + 3);
                CHECK(0, buf);
            }
        }
    }

    // Erase from middle (backward-shift correctness)
    {
        TMSafeHashSet<int> s;
        for (int i = 0; i < 32; i++)
            s.insert(i);
        for (int i = 0; i < 32; i += 2)
            s.erase(i);
        for (int i = 0; i < 32; i++) {
            if (i % 2 == 0) {
                if (s.contains(i)) {
                    char buf[64]; snprintf(buf, sizeof(buf), "erased even %d", i);
                    CHECK(0, buf);
                }
            } else {
                if (!s.contains(i)) {
                    char buf[64]; snprintf(buf, sizeof(buf), "kept odd %d", i);
                    CHECK(0, buf);
                }
            }
        }
        CHECK(s.size() == 16, "half remain after alternating erase");
    }

    if (failures == 0)
        printf("PASS: all TMSafeHashSet tests passed\n");
    return failures > 0 ? 1 : 0;
}
