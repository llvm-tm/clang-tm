/**
 * Test: Local pointer to TM global (demonstrates plugin bug)
 *
 * This test demonstrates the bug where the plugin incorrectly skips
 * instrumentation when a pointer to a TM global is stored in a local variable.
 *
 * The plugin's originatesFromLocal() checks if the pointer variable itself
 * is in LocalVars (allocas), not whether the DATA it points to is local.
 *
 * Expected: Plugin should instrument *p = 42 with tm_write_i4
 * Actual: Plugin skips it because p is stored in a local variable
 */

#include <cstdio>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM int32_t shared_val = 0;

TX void test_local_pointer() {
    int32_t *p = &shared_val;
    *p = 42;
}

MAIN int main() {
    test_local_pointer();
    printf("shared_val = %d (expected 42)\n", shared_val);
    if (shared_val == 42) {
        printf("PASS: Plugin correctly instrumented local pointer\n");
        return 0;
    } else {
        printf("FAIL: Plugin skipped instrumentation (local pointer bug)\n");
        return 1;
    }
}