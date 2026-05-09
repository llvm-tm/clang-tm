/**
 * Test: Pointer arithmetic on TM data
 *
 * Tests the plugin's handling of pointer arithmetic
 * to access elements within TM-annotated arrays.
 */

#include <cstdio>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

TM int32_t tm_array[8] = {1, 2, 3, 4, 5, 6, 7, 8};

TX void sum_elements() {
    int32_t *p = tm_array;
    int32_t sum = 0;
    
    for (int i = 0; i < 8; i++) {
        sum += *(p + i);
    }
    
    tm_array[0] = sum;
}

TX void access_via_offset() {
    tm_array[4] = tm_array[2] + tm_array[3];
}

int main() {
    sum_elements();
    printf("sum = %d (expected 36)\n", tm_array[0]);
    
    access_via_offset();
    printf("array[4] = %d (expected 7)\n", tm_array[4]);
    
    int pass = (tm_array[0] == 36 && tm_array[4] == 7) ? 0 : 1;
    
    if (pass == 0) {
        printf("PASS: pointer arithmetic test\n");
    } else {
        printf("FAIL: pointer arithmetic test\n");
    }
    return pass;
}