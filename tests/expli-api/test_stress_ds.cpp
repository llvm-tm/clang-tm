#include "memory_access.hpp"
#include "explicit_rbtree.hpp"
#include "explicit_sorted_list.hpp"
#include "explicit_hashmap.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <algorithm>
#include <random>
#include <numeric>

static int failures = 0;
static int tests = 0;

#define CHECK(cond, msg) do { \
    tests++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s] %s\n", #cond, msg); \
        failures++; \
    } \
} while(0)

// ─── Helpers ──────────────────────────────────────────────────────

using RNG = std::mt19937;

static void shuffle_vec(std::vector<int>& v, RNG& rng) {
    std::shuffle(v.begin(), v.end(), rng);
}

template<typename K, typename V>
static explicit_rbtree::Node<K,V>* alloc_rb_node(K key, V val) {
    using Node = explicit_rbtree::Node<K,V>;
    auto* n = (Node*)std::malloc(sizeof(Node));
    n->key = key;
    n->val = val;
    n->left = nullptr;
    n->right = nullptr;
    n->parent = nullptr;
    n->color = Node::BLACK;
    return n;
}

template<typename K, typename V>
static explicit_rbtree::Node<K,V>* alloc_rb_node_tm(K key, V val) {
    using Node = explicit_rbtree::Node<K,V>;
    auto* n = (Node*)expli::TM<int>::malloc(sizeof(Node));
    n->key = key;
    n->val = val;
    n->left = nullptr;
    n->right = nullptr;
    n->parent = nullptr;
    n->color = Node::BLACK;
    return n;
}

template<typename T>
static explicit_slist::Node<T>* alloc_sl_node(const T& val) {
    auto* n = (explicit_slist::Node<T>*)std::malloc(sizeof(explicit_slist::Node<T>));
    n->data = val;
    n->next = nullptr;
    return n;
}

template<typename T>
static explicit_slist::Node<T>* alloc_sl_node_tm(const T& val) {
    auto* n = (explicit_slist::Node<T>*)expli::TM<int>::malloc(sizeof(explicit_slist::Node<T>));
    n->data = val;
    n->next = nullptr;
    return n;
}

// ─── RBTree: standalone (UseTM=false) ──────────────────────────────

static void test_rbtree_standalone() {
    explicit_rbtree::Tree<int, int> tree;

    std::vector<int> keys(5000);
    std::iota(keys.begin(), keys.end(), 0);
    RNG rng(42);
    shuffle_vec(keys, rng);

    std::vector<explicit_rbtree::Node<int,int>*> nodes;
    for (int k : keys) {
        auto* n = alloc_rb_node(k, k * 10);
        CHECK(explicit_rbtree::insert<false>(&tree, n) == nullptr,
              "rbtree insert new");
        nodes.push_back(n);
    }

    for (int k : keys) {
        CHECK(explicit_rbtree::contains<false>(&tree, k),
              "rbtree contains after insert");
        int* v = explicit_rbtree::find<false>(&tree, k);
        CHECK(v && *v == k * 10, "rbtree find correct value");
    }

    auto* dup = alloc_rb_node(keys[0], 999);
    CHECK(explicit_rbtree::insert<false>(&tree, dup) != nullptr,
          "rbtree duplicate insert returns existing");
    std::free(dup);

    CHECK(!explicit_rbtree::contains<false>(&tree, -1),
          "rbtree absent key");
    CHECK(explicit_rbtree::find<false>(&tree, -1) == nullptr,
          "rbtree find absent returns null");

    int prev = -1;
    for (int k = 0; k < 5000; k++) {
        auto* n = explicit_rbtree::lookup<false>(&tree, k);
        CHECK(n != nullptr, "rbtree lookup sequential");
        CHECK(n->key > prev, "rbtree keys in order");
        prev = n->key;
    }

    for (auto* n : nodes) std::free(n);
    printf("  RBTree standalone:    %s\n", failures ? "FAIL" : "PASS");
}

// ─── RBTree: inside TX (UseTM=true) ───────────────────────────────

static void test_rbtree_tx() {
    auto* tree = (explicit_rbtree::Tree<int, int>*)expli::TM<int>::malloc(
        sizeof(explicit_rbtree::Tree<int, int>));
    tree->root = nullptr;

    expli::TM<int>::transaction([&]() {
        auto* n1 = alloc_rb_node_tm(10, 100);
        auto* n2 = alloc_rb_node_tm(20, 200);
        auto* n3 = alloc_rb_node_tm(5, 50);

        CHECK(explicit_rbtree::insert<true>(tree, n1) == nullptr,
              "rbtree tx insert n1");
        CHECK(explicit_rbtree::insert<true>(tree, n2) == nullptr,
              "rbtree tx insert n2");
        CHECK(explicit_rbtree::insert<true>(tree, n3) == nullptr,
              "rbtree tx insert n3");

        CHECK(explicit_rbtree::contains<true>(tree, 10), "rbtree tx contains 10");
        CHECK(explicit_rbtree::contains<true>(tree, 20), "rbtree tx contains 20");
        CHECK(explicit_rbtree::contains<true>(tree, 5), "rbtree tx contains 5");
        CHECK(!explicit_rbtree::contains<true>(tree, 99), "rbtree tx not contains 99");
    });

    CHECK(explicit_rbtree::contains<false>(tree, 10), "rbtree post-tx contains 10");
    CHECK(explicit_rbtree::contains<false>(tree, 5), "rbtree post-tx contains 5");

    printf("  RBTree inside TX:    %s\n", failures ? "FAIL" : "PASS");
}

// ─── SortedList: standalone (UseTM=false) ──────────────────────────

static void test_slist_standalone() {
    explicit_slist::List<int> list;

    std::vector<int> keys(5000);
    std::iota(keys.begin(), keys.end(), 0);
    RNG rng(123);
    shuffle_vec(keys, rng);

    for (int k : keys) {
        auto* n = alloc_sl_node(k);
        CHECK(explicit_slist::insert<false>(&list, n),
              "slist insert");
    }

    for (int k : keys) {
        CHECK(explicit_slist::contains<false>(&list, k),
              "slist contains after insert");
    }

    int prev = -1;
    explicit_slist::Node<int>* cur = list.head;
    while (cur) {
        CHECK(cur->data > prev, "slist sorted order");
        prev = cur->data;
        cur = cur->next;
    }
    CHECK(prev == 4999, "slist max element at end");

    CHECK(explicit_slist::remove<false>(&list, 0), "slist remove first");
    CHECK(!explicit_slist::contains<false>(&list, 0), "slist removed first");
    CHECK(explicit_slist::remove<false>(&list, 4999), "slist remove last");
    CHECK(!explicit_slist::contains<false>(&list, 4999), "slist removed last");
    CHECK(explicit_slist::remove<false>(&list, 2500), "slist remove middle");
    CHECK(!explicit_slist::contains<false>(&list, 2500), "slist removed middle");

    CHECK(!explicit_slist::remove<false>(&list, -1), "slist remove absent");

    cur = list.head;
    while (cur) {
        auto* next = cur->next;
        std::free(cur);
        cur = next;
    }

    printf("  SortedList standalone: %s\n", failures ? "FAIL" : "PASS");
}

// ─── HashMap: standalone (UseTM=false) ────────────────────────────

static void test_hashmap_standalone() {
    explicit_hashmap::Map<int, int> map;
    size_t cap = 16384;
    map.slots = (explicit_hashmap::Slot<int,int>*)std::calloc(cap, sizeof(explicit_hashmap::Slot<int,int>));
    map.capacity = cap;
    map.size = 0;

    std::vector<int> keys(10000);
    std::iota(keys.begin(), keys.end(), 0);
    for (auto& k : keys) k = k * 7 + 3;
    RNG rng(77);
    shuffle_vec(keys, rng);

    for (int k : keys) {
        CHECK(explicit_hashmap::insert<false>(&map, k, k * 2),
              "hashmap insert");
    }

    for (int k : keys) {
        int* v = explicit_hashmap::find<false>(&map, k);
        CHECK(v != nullptr, "hashmap find after insert");
        CHECK(*v == k * 2, "hashmap find correct value");
    }

    CHECK(!explicit_hashmap::insert<false>(&map, keys[0], 999),
          "hashmap update returns false");
    int* v = explicit_hashmap::find<false>(&map, keys[0]);
    CHECK(v && *v == 999, "hashmap updated value");

    CHECK(explicit_hashmap::erase<false>(&map, keys[0]),
          "hashmap erase");
    CHECK(explicit_hashmap::find<false>(&map, keys[0]) == nullptr,
          "hashmap erased not found");
    CHECK(explicit_hashmap::erase<false>(&map, keys[0]) == false,
          "hashmap erase absent returns false");

    CHECK(map.size == 9999, "hashmap size after erase");

    std::free(map.slots);
    printf("  HashMap standalone:   %s\n", failures ? "FAIL" : "PASS");
}

// ─── HashMap: inside TX (UseTM=true) ──────────────────────────────

static void test_hashmap_tx() {
    auto* map = (explicit_hashmap::Map<int, int>*)expli::TM<int>::malloc(
        sizeof(explicit_hashmap::Map<int, int>));
    size_t cap = 4096;
    map->slots = (explicit_hashmap::Slot<int,int>*)expli::TM<int>::calloc(cap, sizeof(explicit_hashmap::Slot<int,int>));
    map->capacity = cap;
    map->size = 0;

    expli::TM<int>::transaction([&]() {
        for (int i = 0; i < 2000; i++) {
            CHECK(explicit_hashmap::insert<true>(map, i, i * 3),
                  "hashmap tx insert");
        }

        // Inside TX: read through TM (write-back hasn't committed to memory yet)
        for (int i = 0; i < 2000; i++) {
            int* v = explicit_hashmap::find<true>(map, i);
            CHECK(v != nullptr, "hashmap tx find");
            int val = MemoryAccess<true>::load(v);
            CHECK(val == i * 3, "hashmap tx find value");
        }

        CHECK(explicit_hashmap::erase<true>(map, 500), "hashmap tx erase");
        CHECK(explicit_hashmap::find<true>(map, 500) == nullptr,
              "hashmap tx erased not found");
    });

    // After TX: values committed to memory, plain load works
    for (int i = 0; i < 2000; i++) {
        if (i == 500) continue; // erased
        int* v = explicit_hashmap::find<false>(map, i);
        CHECK(v != nullptr, "hashmap post-tx find");
        CHECK(*v == i * 3, "hashmap post-tx value");
    }
    CHECK(explicit_hashmap::find<false>(map, 500) == nullptr,
          "hashmap post-tx erased not found");
    printf("  HashMap inside TX:    %s\n", failures ? "FAIL" : "PASS");
}

// ─── Multi-threaded stress ────────────────────────────────────────

static void stress_rbtree_thread(explicit_rbtree::Tree<int,int>* tree,
                                  const std::vector<int>& my_keys) {
    expli::TM<int>::thread_init();

    for (int k : my_keys) {
        auto* n = alloc_rb_node_tm(k, k * 10);
        expli::TM<int>::transaction([&]() {
            explicit_rbtree::insert<true>(tree, n);
        });
    }

    for (int k : my_keys) {
        expli::TM<int>::transaction([&]() {
            CHECK(explicit_rbtree::contains<true>(tree, k),
                  "stress rbtree contains own");
        });
    }

    expli::TM<int>::thread_exit();
}

static void test_rbtree_multithread() {
    const int NTH = 4;
    const int OPS = 2000;

    auto* tree = (explicit_rbtree::Tree<int, int>*)expli::TM<int>::malloc(
        sizeof(explicit_rbtree::Tree<int, int>));
    tree->root = nullptr;

    std::vector<std::thread> threads;
    std::vector<std::vector<int>> thread_keys(NTH);

    for (int t = 0; t < NTH; t++) {
        for (int i = 0; i < OPS; i++)
            thread_keys[t].push_back(t * OPS + i);
        RNG rng(t * 1000 + 42);
        shuffle_vec(thread_keys[t], rng);
    }

    for (int t = 0; t < NTH; t++)
        threads.emplace_back(stress_rbtree_thread, tree, thread_keys[t]);
    for (auto& th : threads) th.join();

    for (int t = 0; t < NTH; t++)
        for (int k : thread_keys[t])
            CHECK(explicit_rbtree::contains<false>(tree, k),
                  "rbtree multi-threaded contains all");

    int prev = -1;
    int count = 0;
    for (int k = 0; k < NTH * OPS; k++) {
        auto* n = explicit_rbtree::lookup<false>(tree, k);
        CHECK(n != nullptr, "rbtree multi-threaded sequential lookup");
        CHECK(n->key > prev, "rbtree multi-threaded order");
        prev = n->key;
        count++;
    }
    CHECK(count == NTH * OPS, "rbtree multi-threaded all present");

    printf("  RBTree multi-threaded: %s\n", failures ? "FAIL" : "PASS");
}

static void stress_hashmap_thread(explicit_hashmap::Map<int,int>* map,
                                   const std::vector<int>& my_keys) {
    expli::TM<int>::thread_init();

    for (int k : my_keys) {
        expli::TM<int>::transaction([&]() {
            explicit_hashmap::insert<true>(map, k, k * 2);
        });
    }

    for (int k : my_keys) {
        expli::TM<int>::transaction([&]() {
            int* v = explicit_hashmap::find<true>(map, k);
            CHECK(v != nullptr && *v == k * 2,
                  "stress hashmap find own");
        });
    }

    expli::TM<int>::thread_exit();
}

static void test_hashmap_multithread() {
    const int NTH = 4;
    const int OPS = 2000;

    auto* map = (explicit_hashmap::Map<int, int>*)expli::TM<int>::malloc(
        sizeof(explicit_hashmap::Map<int, int>));
    size_t cap = 16384;
    map->slots = (explicit_hashmap::Slot<int,int>*)expli::TM<int>::calloc(cap, sizeof(explicit_hashmap::Slot<int,int>));
    map->capacity = cap;
    map->size = 0;

    std::vector<std::thread> threads;
    std::vector<std::vector<int>> thread_keys(NTH);

    for (int t = 0; t < NTH; t++) {
        for (int i = 0; i < OPS; i++)
            thread_keys[t].push_back(t * OPS + i);
        RNG rng(t * 777 + 42);
        shuffle_vec(thread_keys[t], rng);
    }

    for (int t = 0; t < NTH; t++)
        threads.emplace_back(stress_hashmap_thread, map, thread_keys[t]);
    for (auto& th : threads) th.join();

    for (int t = 0; t < NTH; t++)
        for (int k : thread_keys[t]) {
            int* v = explicit_hashmap::find<false>(map, k);
            CHECK(v != nullptr && *v == k * 2,
                  "hashmap multi-threaded verify");
        }
    printf("  HashMap multi-threaded: %s\n", failures ? "FAIL" : "PASS");
}

// ─── Main ─────────────────────────────────────────────────────────

int main() {
    expli::TM<int>::init();
    expli::TM<int>::thread_init();

    printf("Data structure stress tests\n");
    printf("===========================\n\n");

    test_rbtree_standalone();
    test_rbtree_tx();
    test_slist_standalone();
    test_hashmap_standalone();
    test_hashmap_tx();
    test_rbtree_multithread();
    test_hashmap_multithread();

    expli::TM<int>::exit();

    printf("\n");
    if (failures) {
        printf("FAILED: %d/%d tests failed\n", failures, tests);
        return 1;
    }
    printf("ALL %d tests PASSED\n", tests);
    return 0;
}
