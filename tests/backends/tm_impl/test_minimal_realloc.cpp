#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstdlib>

struct Elem {
    int id;
    int type;
    int pad1, pad2, pad3, pad4;
    std::vector<int> ids;

    Elem() : id(0), type(0), pad1(0), pad2(0), pad3(0), pad4(0) {}
    Elem(int i, int t) : id(i), type(t), pad1(0), pad2(0), pad3(0), pad4(0) {
        ids.push_back(i);
    }
};

static std::vector<Elem> g_vec;

__attribute__((annotate("shared"), noinline))
void push_tx() {
    g_vec.push_back(Elem(999999, 1));
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 10000;

    g_vec.resize(n);
    for (int i = 0; i < n; i++)
        g_vec[i] = Elem(i, i % 5);
    printf("init %d elems, sizeof=%zu\n", n, sizeof(Elem));

    tm_init();
    tm_init_thread();
    tm_begin();
    push_tx();
    tm_end();
    printf("done, size=%zu\n", g_vec.size());

    // Verify
    for (int i = 0; i < (int)g_vec.size() - 1; i++) {
        if (g_vec[i].id != i) {
            printf("FAIL: id mismatch at %d: got %d\n", i, g_vec[i].id);
            return 1;
        }
        if (!g_vec[i].ids.empty() && g_vec[i].ids[0] != i) {
            printf("FAIL: ids[0] mismatch at %d: got %d\n", i, g_vec[i].ids[0]);
            return 1;
        }
    }
    if (g_vec.back().id != 999999) {
        printf("FAIL: last element id mismatch: %d\n", g_vec.back().id);
        return 1;
    }
    printf("ALL OK\n");
    tm_exit_thread();
    return 0;
}
