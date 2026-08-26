// Tests for expli::vector and expli::flat_set used in benchmark STL replacement.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../explicit_api/cpp/include/tm_api.hpp"
#include "../../explicit_api/cpp/include/tm_map.hpp"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        failures++; \
    } \
} while(0)

int main()
{
    // ── expli::vector tests ──
    {
        expli::vector<int> v;
        CHECK(v.empty(), "new vector is empty");
        CHECK(v.size() == 0, "new vector size 0");

        v.push_back(10);
        v.push_back(20);
        v.push_back(30);
        CHECK(v.size() == 3, "size 3 after push_back");
        CHECK(v[0] == 10 && v[1] == 20 && v[2] == 30, "index access");
        CHECK(v.back() == 30, "back()");
        CHECK(v.front() == 10, "front()");
        CHECK(!v.empty(), "not empty");

        // range-for
        int sum = 0;
        for (int x : v) sum += x;
        CHECK(sum == 60, "range-for sum");

        v.pop_back();
        CHECK(v.size() == 2, "size 2 after pop_back");
        CHECK(v.back() == 20, "back after pop");

        v.clear();
        CHECK(v.empty(), "empty after clear");
    }

    // ── expli::vector with size/init ──
    {
        expli::vector<char> visited(10);
        CHECK(visited.size() == 10, "size-constructed vector");
        visited[3] = 1;
        CHECK(visited[3] == 1, "element write via []");
        CHECK(visited[0] == 0, "default-initialized element");
    }

    // ── expli::flat_set tests ──
    {
        expli::flat_set<int> s;
        CHECK(s.empty(), "new set empty");
        CHECK(!s.contains(1), "empty set no contain");

        s.insert(5);
        s.insert(3);
        s.insert(7);
        s.insert(1);
        CHECK(s.size() == 4, "set size 4");
        CHECK(s.contains(5), "contains inserted");
        CHECK(s.contains(3), "contains inserted");
        CHECK(s.contains(1), "contains inserted");
        CHECK(!s.contains(0), "no contain non-inserted");
        CHECK(!s.contains(6), "no contain non-inserted");

        // Duplicate
        s.insert(3);
        CHECK(s.size() == 4, "dup doesn't increase size");

        // Erase
        s.erase(3);
        CHECK(s.size() == 3, "size 3 after erase");
        CHECK(!s.contains(3), "erased value gone");
        CHECK(s.contains(5), "others remain");
        CHECK(s.contains(7), "others remain");
        CHECK(s.contains(1), "others remain");

        // Clear
        s.clear();
        CHECK(s.empty(), "empty after clear");

        // Range-for over set
        s.insert(10);
        s.insert(30);
        s.insert(20);
        int prev = -1;
        for (int x : s) {
            CHECK(x > prev, "set iteration in order");
            prev = x;
        }
    }

    // ── expli::flat_set with custom struct (Edge-like) ──
    {
        struct Point { double x, y; };
        struct Edge {
            Point a, b;
            bool operator<(const Edge& o) const {
                if (a.x != o.a.x) return a.x < o.a.x;
                if (a.y != o.a.y) return a.y < o.a.y;
                if (b.x != o.b.x) return b.x < o.b.x;
                return b.y < o.b.y;
            }
        };

        expli::flat_set<Edge> edges;
        edges.insert({{1,2},{3,4}});
        edges.insert({{5,6},{7,8}});
        CHECK(edges.size() == 2, "edge set size");
        CHECK(edges.contains({{1,2},{3,4}}), "edge set contains");
        CHECK(!edges.contains({{9,10},{11,12}}), "edge set no contain");

        // range-for
        int count = 0;
        for (const auto& e : edges) count++;
        CHECK(count == 2, "edge set range-for count");
    }

    if (failures == 0)
        printf("PASS: all expli container tests passed\n");
    return failures > 0 ? 1 : 0;
}
