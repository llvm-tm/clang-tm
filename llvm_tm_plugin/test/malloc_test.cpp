/**
 * Test: malloc and heap-allocated TM data
 *
 * Tests the plugin's handling of heap-allocated memory
 * that is accessed through TM-annotated pointers.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

TM int32_t *heap_data = nullptr;

TX void init_data() {
    heap_data = (int32_t *)malloc(sizeof(int32_t) * 4);
    heap_data[0] = 10;
    heap_data[1] = 20;
    heap_data[2] = 30;
    heap_data[3] = 40;
}

TX void access_data() {
    int32_t sum = heap_data[0] + heap_data[1] + heap_data[2] + heap_data[3];
    heap_data[0] = sum;
}

TX void cleanup_data() {
    free(heap_data);
    heap_data = nullptr;
}

int main() {
    init_data();
    access_data();
    
    printf("heap_data[0] = %d (expected 100)\n", heap_data[0]);
    
    int pass = (heap_data[0] == 100) ? 0 : 1;
    
    cleanup_data();
    
    if (pass == 0) {
        printf("PASS: heap test\n");
    } else {
        printf("FAIL: heap test\n");
    }
    return pass;
}