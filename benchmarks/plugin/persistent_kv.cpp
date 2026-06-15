/**
 * Persistent KV Store — demonstrates TM persistence between runs
 *
 * Uses fixed-size TM-annotated arrays so data survives in the mmap file.
 * Run repeatedly: each invocation increments the counter and records
 * the previous value.
 *
 * Build:
 *   cd benchmarks && mkdir -p out bin
 *   clang++ -O3 -fno-inline -emit-llvm -S persistent_kv.cpp -o out/persistent_kv.ll
 *   opt -load-pass-plugin=../plugin/bin/libTMInstrument.so \
 *       -passes="tm-instrument" out/persistent_kv.ll -o out/persistent_kv.instr.ll
 *   opt -O3 out/persistent_kv.instr.ll -o out/persistent_kv.opt.bc
 *   clang++ -O3 out/persistent_kv.opt.bc \
 *       ../backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp -o bin/persistent_kv
 *
 * Run:
 *   ./bin/persistent_kv    # first run: counter=0
 *   ./bin/persistent_kv    # second run: counter=1 (restored)
 */

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

#include <cstdio>
#include <cstring>

constexpr int MAX_ENTRIES = 1024;

// NOTE: std::map / std::vector use heap-allocated nodes that do NOT
// survive across runs.  The PersistentSGL backend only persists the
// TM symbol's own memory (the struct/array itself), not heap data.
// Fixed-size arrays avoid this limitation.

TM char     kv_keys[MAX_ENTRIES][64];
TM int      kv_values[MAX_ENTRIES];
TM int      kv_count = 0;

TX void kv_set(const char* key, int value) {
    for (int i = 0; i < kv_count; i++) {
        if (strcmp(kv_keys[i], key) == 0) {
            kv_values[i] = value;
            return;
        }
    }
    int n = kv_count;
    int i = 0;
    while (key[i] && i < 63) { kv_keys[n][i] = key[i]; i++; }
    kv_keys[n][i] = 0;
    kv_values[n] = value;
    kv_count = n + 1;
}

TX int kv_get(const char* key) {
    for (int i = 0; i < kv_count; i++)
        if (strcmp(kv_keys[i], key) == 0)
            return kv_values[i];
    return -1;
}

TX int kv_delete(const char* key) {
    for (int i = 0; i < kv_count; i++) {
        if (strcmp(kv_keys[i], key) == 0) {
            int old = kv_values[i];
            for (int j = i; j < kv_count - 1; j++) {
                int k = 0;
                do { kv_keys[j][k] = kv_keys[j+1][k]; k++; } while (kv_keys[j][k]);
                kv_values[j] = kv_values[j+1];
            }
            kv_count--;
            return old;
        }
    }
    return -1;
}

TX void kv_dump() {
    printf("KV store (%d entries):\n", kv_count);
    for (int i = 0; i < kv_count && i < 10; i++)
        printf("  '%s' = %d\n", kv_keys[i], kv_values[i]);
    if (kv_count > 10) printf("  ... (%d more)\n", kv_count - 10);
}

MAIN int main() {
    printf("=== Persistent KV Store ===\n");

    int v = kv_get("counter");
    printf("Previous counter: %d\n", v);

    kv_set("counter", v + 1);
    kv_set("last_run", v);

    int prev = kv_get("counter");
    kv_dump();
    printf("Current counter:  %d\n", prev);
    printf("Next run will see previous = %d\n", prev);
    return 0;
}
