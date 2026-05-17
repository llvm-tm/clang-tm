#include <cstdio>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

struct TM IntVector {
    std::vector<int> items;
};

TX void push_items(IntVector* data, int count) {
    for (int i = 0; i < count; i++) {
        data->items.push_back(i);
    }
}

TX void read_items(IntVector* data) {
    int sum = 0;
    for (size_t i = 0; i < data->items.size(); i++) {
        sum += data->items[i];
    }
    if (data->items.size() > 0) {
        data->items[0] = sum;
    }
}

TM IntVector g_vec;

THREAD void worker(int tid) {
    push_items(&g_vec, 50);
}

MAIN int main() {
    printf("TM argument trace test\n");
    printf("======================\n\n");

    push_items(&g_vec, 50);
    read_items(&g_vec);

    printf("  g_vec.items.size() = %zu\n", g_vec.items.size());
    printf("  g_vec.items[0] = %d (expected 1225)\n", g_vec.items[0]);

    bool pass = (g_vec.items.size() == 50 && g_vec.items[0] == 1225);
    printf("\n%s\n", pass ? "PASS: argument trace test passed" : "FAIL: argument trace test failed");
    return pass ? 0 : 1;
}
