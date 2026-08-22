/**
 * Minimal single-threaded POWER8 HTM test.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline int tbegin(void) {
    int cr;
    __asm__ __volatile__(
        "tbegin. 0\n\t"
        "mfocrf %0, 2\n\t"
        : "=r"(cr)
        :
        : "cr0", "memory");
    return (cr & (1 << 2)) != 0;
}

static inline void tend(void) {
    __asm__ __volatile__("tend. 0\n\t" ::: "memory");
}

static inline void tabort(void) {
    __asm__ __volatile__("tabort. 0\n\t" ::: "memory");
}

int main(void) {
    uint64_t counter = 0;
    int commits = 0, aborts = 0;

    for (int i = 0; i < 5; i++) {
        if (tbegin()) {
            /* Inside transaction */
            uint64_t v = counter;
            v += 1;
            counter = v;
            tend();
            commits++;
        } else {
            aborts++;
        }
    }

    printf("commits=%d aborts=%d counter=%llu\n",
           commits, aborts, (unsigned long long)counter);

    if (counter == 5) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: expected counter=5, got %llu\n",
           (unsigned long long)counter);
    return 1;
}
