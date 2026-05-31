// Unit tests for explicit TM API data structures
// Compiles and runs WITHOUT any TM runtime — standalone.
#include "../tm_api.hpp"
#include "../tm_map.hpp"
#include <cstdio>
#include <cstring>
#include <string>  // for test comparison only

static int failures = 0;
static int tests    = 0;

#define CHECK(cond, msg) do { \
    tests++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s] %s\n", #cond, msg); \
        failures++; \
    } \
} while(0)

// ── Test my::pair ───────────────────────────────────────────
static void test_pair() {
    // Basic construction
    expli::pair<int,double> p1(42, 3.14);
    CHECK(p1.first == 42, "pair.first");
    CHECK(p1.second > 3.13 && p1.second < 3.15, "pair.second");

    // Default construction
    expli::pair<int,int> p2;
    CHECK(p2.first == 0 && p2.second == 0, "pair default init");

    // make_pair
    auto p3 = expli::make_pair(1, 'a');
    CHECK(p3.first == 1 && p3.second == 'a', "make_pair");

    // Copy construct
    expli::pair<int,char> p4(p3);
    CHECK(p4.first == 1 && p4.second == 'a', "pair copy ctor");
}

// ── Test my::vector ─────────────────────────────────────────
static void test_vector() {
    // Empty vector
    expli::vector<int> v;
    CHECK(v.empty(), "vector empty");
    CHECK(v.size() == 0, "vector size 0");

    // push_back
    for (int i = 0; i < 100; i++) v.push_back(i);
    CHECK(v.size() == 100, "vector size after push_back");
    CHECK(!v.empty(), "vector not empty");

    // element access
    for (int i = 0; i < 100; i++) CHECK(v[i] == i, "vector element access");

    // iteration
    int sum = 0;
    for (auto it = v.begin(); it != v.end(); ++it) sum += *it;
    CHECK(sum == 100*99/2, "vector iteration sum");

    // range-for
    sum = 0;
    for (int x : v) sum += x;
    CHECK(sum == 100*99/2, "vector range-for sum");

    // clear
    v.clear();
    CHECK(v.empty(), "vector after clear");

    // resize
    v.resize(50);
    CHECK(v.size() == 50, "vector resize 50");
    v.resize(100);
    CHECK(v.size() == 100, "vector resize 100");
    // resized elements should be default-initialized
    for (int i = 50; i < 100; i++) CHECK(v[i] == 0, "vector resize default");

    // front / back
    v[0] = 10;
    v[99] = 20;
    CHECK(v.front() == 10, "vector front");
    CHECK(v.back() == 20, "vector back");

    // pop_back
    v.pop_back();
    CHECK(v.size() == 99, "vector after pop_back");

    // data pointer
    CHECK(v.data() != nullptr, "vector data");
    CHECK(v.data()[0] == 10, "vector data[0]");

    // reserve + capacity
    v.reserve(200);
    CHECK(v.capacity() >= 200, "vector capacity after reserve");

    // Copy construction
    expli::vector<int> v2(v);
    CHECK(v2.size() == v.size(), "vector copy size");
    CHECK(v2[0] == v[0], "vector copy element");
    v2[0] = 999;
    CHECK(v[0] == 10, "vector copy deep (independence)");
}

// ── Test my::vector<string> ─────────────────────────────────
static void test_vector_string() {
    expli::vector<expli::string> v;
    v.push_back(expli::string("hello"));
    v.push_back(expli::string("world"));
    CHECK(v.size() == 2, "vector<string> size");
    CHECK(strcmp(v[0].c_str(), "hello") == 0, "vector<string> element 0");
    CHECK(strcmp(v[1].c_str(), "world") == 0, "vector<string> element 1");

    v.resize(1);
    CHECK(v.size() == 1, "vector<string> after resize");
}

// ── Test flat_map ───────────────────────────────────────────
static void test_flat_map() {
    expli::flat_map<int, int> m;

    CHECK(m.empty(), "map empty");
    CHECK(m.find(1) == nullptr, "map find absent");

    m.insert(5, 50);
    m.insert(2, 20);
    m.insert(8, 80);
    m.insert(1, 10);
    m.insert(3, 30);

    CHECK(m.size() == 5, "map size after inserts");
    CHECK(!m.empty(), "map not empty");

    int *v = m.find(1);
    CHECK(v != nullptr && *v == 10, "map find 1");
    v = m.find(5);
    CHECK(v != nullptr && *v == 50, "map find 5");
    v = m.find(8);
    CHECK(v != nullptr && *v == 80, "map find 8");
    v = m.find(99);
    CHECK(v == nullptr, "map find absent");

    // Overwrite
    m.insert(5, 55);
    v = m.find(5);
    CHECK(v != nullptr && *v == 55, "map overwrite");

    // Erase
    m.erase(2);
    CHECK(m.size() == 4, "map size after erase");
    CHECK(m.find(2) == nullptr, "map find erased");

    m.erase(99);
    CHECK(m.size() == 4, "map size after erase-absent");

    // operator[]
    m[10] = 100;
    v = m.find(10);
    CHECK(v != nullptr && *v == 100, "map operator[] insert");

    m[10] = 200;
    v = m.find(10);
    CHECK(v != nullptr && *v == 200, "map operator[] update");

    // Clear
    m.clear();
    CHECK(m.empty(), "map after clear");
}

// ── Test flat_multimap ──────────────────────────────────────
static void test_flat_multimap() {
    expli::flat_multimap<int, int> mm;

    mm.insert(1, 10);
    mm.insert(2, 20);
    mm.insert(1, 11);  // duplicate key

    int *v = mm.find_first(1);
    CHECK(v != nullptr && (*v == 10 || *v == 11), "multimap find_first");

    v = mm.find_first(2);
    CHECK(v != nullptr && *v == 20, "multimap find_first 2");

    v = mm.find_first(99);
    CHECK(v == nullptr, "multimap find_first absent");
}

// ── Test flat_set ───────────────────────────────────────────
static void test_flat_set() {
    expli::flat_set<int> s;

    CHECK(!s.contains(1), "set not contains 1");
    s.insert(5);
    s.insert(2);
    s.insert(8);
    s.insert(1);

    CHECK(s.contains(1), "set contains 1");
    CHECK(s.contains(5), "set contains 5");
    CHECK(s.contains(8), "set contains 8");
    CHECK(!s.contains(3), "set not contains 3");

    // Duplicate insert
    s.insert(5);
    CHECK(s.size() == 4, "set no duplicates");

    s.clear();
    CHECK(s.size() == 0, "set after clear");
}

// ── Test TM<int> peek/poke ──────────────────────────────────
static void test_tm_peek_poke() {
    expli::TM<int> x;
    CHECK(x.peek() == 0, "TM<int> default init");

    x.poke(42);
    CHECK(x.peek() == 42, "TM<int> poke");

    x.poke(-1);
    CHECK(x.peek() == -1, "TM<int> poke negative");
}

// ── Test TM<std::pair> composition ──────────────────────────
static void test_tm_pair_composition() {
    // struct with TM fields
    struct Widget {
        expli::TM<int> id;
        expli::TM<double> value;
    };

    Widget w;
    w.id.poke(100);
    w.value.poke(3.14);
    CHECK(w.id.peek() == 100, "TM field 1");
    CHECK(w.value.peek() > 3.13 && w.value.peek() < 3.15, "TM field 2");
}

// ── Main ────────────────────────────────────────────────────
int main() {
    printf("Data structure unit tests\n");
    printf("========================\n\n");

    test_pair();             printf("  my::pair:       %s\n", failures ? "FAIL" : "PASS");
    test_vector();           printf("  my::vector<int>: %s\n", failures ? "FAIL" : "PASS");
    test_vector_string();    printf("  my::vector<string>: %s\n", failures ? "FAIL" : "PASS");
    test_flat_map();         printf("  flat_map:       %s\n", failures ? "FAIL" : "PASS");
    test_flat_multimap();    printf("  flat_multimap:  %s\n", failures ? "FAIL" : "PASS");
    test_flat_set();         printf("  flat_set:       %s\n", failures ? "FAIL" : "PASS");
    test_tm_peek_poke();     printf("  TM<int> peek:   %s\n", failures ? "FAIL" : "PASS");
    test_tm_pair_composition(); printf("  TM composition: %s\n", failures ? "FAIL" : "PASS");

    printf("\n");
    if (failures) {
        printf("FAILED: %d/%d tests failed\n", failures, tests);
        return 1;
    }
    printf("ALL %d tests PASSED\n", tests);
    return 0;
}
