#include <cstdint>
#include <cstdio>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// Use __int128 to force a true 16-byte LLVM type
TM __int128 g_i128_val = 0;

TX void test_i128_write() {
    __int128 val = 0xDEADBEEFCAFEBABELL;
    val = (val << 64) | 0x1234567890ABCDEFLL;
    g_i128_val = val;  // this should trigger the i128 fallback in the plugin
}

TX int test_i128_read() {
    __int128 v = g_i128_val;
    uint64_t lo = (uint64_t)v;
    uint64_t hi = (uint64_t)(v >> 64);
    printf("  lo=0x%llx hi=0x%llx\n", (unsigned long long)lo, (unsigned long long)hi);
    if (lo == 0x1234567890ABCDEFLL && hi == 0xDEADBEEFCAFEBABELL) {
        printf("PASS: i128 read/write\n");
        return 0;
    }
    printf("FAIL: expected lo=0x1234567890abcdef hi=0xdeadbeefcafebabe\n");
    return 1;
}

MAIN int main() {
    printf("=== Test i128 TM store/load ===\n");
    test_i128_write();
    int rc = test_i128_read();
    printf("main done: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
