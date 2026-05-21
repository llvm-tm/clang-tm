#include <cstdint>
#include <cstdio>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))

TM std::vector<int64_t> g_vec;

TX void push_elements(int base, int n) {
    for (int i = 0; i < n; i++)
        g_vec.push_back(base + i);
}

int main() {
    push_elements(0, 400);
    bool ok = true;
    for (size_t i = 0; i < g_vec.size(); i++) {
        if (g_vec[i] != (int64_t)i) {
            printf("  FAIL: g_vec[%zu] = %lld, expected %lld\n",
                   i, (long long)g_vec[i], (long long)i);
            ok = false;
        }
    }
    if (ok) printf("PASS\n");
    return ok ? 0 : 1;
}
