// test_opaque_race.cpp
//
// PURPOSE
// =======
// Demonstrate that `tm_allow_opaque` allows external function calls
// inside a TX function whose IR the pass cannot see (e.g. std::log,
// strncmp, operator new[], memcpy).  The loads/stores of the annotated
// function ARE still instrumented — only the opaque callee check is
// skipped.
//
// WITHOUT tm_allow_opaque the pass rejects external calls in TX context.
// WITH tm_allow_opaque the pass trusts the programmer that the callee
// is safe (or that the programmer has arranged for safety by copying
// shared data into locals before the call).
//
// USAGE IN BENCHMARKS
// ===================
//   bayes:    0 opaque (std::log is external libm — no annotation needed)
//   genome:   1 opaque (genome_match — operator new[], strncmp/strcpy)
//   intruder: 1 opaque (process_decoder — byte tm_memcpy loop)
//   yada:     1 opaque (yada_apply_refinement — new double[])
//
// Each is reviewed to ensure shared TM data is accessed only outside
// the opaque boundary (the copy-to-locals pattern).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "tm_test_common.hpp"

TM struct SharedData {
    double value;
    char   buffer[64];
};

SharedData g_data = {42.0, "hello"};

// ── TX without opaque: pass instruments all loads/stores. ───────────
// External library calls (e.g. std::log) are known-safe — the pass
// allows them without annotation.
TX static void compute_direct(double a) {
    double v = g_data.value;
    v = v + log(a);                              // log is external → safe
    g_data.value = v;
}

// ── TX with opaque: pass skips the opaque-call check. ───────────────
// The loads/stores (g_data.value, g_data.buffer) are STILL instrumented.
// The annotation only says: "trust me, the calls I make are safe".
TX __attribute__((annotate("tm_allow_opaque")))
static void copy_with_opaque(const char* src) {
    double v = g_data.value;                     // instrumented load
    memcpy(g_data.buffer, src, strlen(src) + 1); // memcpy is opaque
    g_data.value = v + 1.0;                      // instrumented store
}

// ── Worker helpers ──────────────────────────────────────────────────
THREAD static void worker_direct() {
    for (int i = 0; i < 1000; i++) compute_direct(1.0 + i);
}

THREAD static void worker_opaque() {
    for (int i = 0; i < 1000; i++) copy_with_opaque("world");
}

MAIN int main() {
    printf("opaque_race: tm_allow_opaque allows external calls in TX\n");
    printf("  (loads/stores of the annotated function ARE instrumented)\n");
    fflush(stdout);

    std::thread t1(worker_direct);
    std::thread t2(worker_direct);
    t1.join(); t2.join();

    std::thread t3(worker_opaque);
    std::thread t4(worker_opaque);
    t3.join(); t4.join();

    printf("  g_data.value  = %f\n", g_data.value);
    printf("  g_data.buffer = \"%s\"\n", g_data.buffer);
    printf("opaque_race: done\n");
    fflush(stdout);
    return 0;
}
