#include <cstring>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

TM char tm_buf1[16];
TM char tm_buf2[16];
TM int32_t tm_int_buf[4];
char non_tm_buf[16];

TX void tm_memops() {
    memset(tm_buf1, 0xAB, sizeof(tm_buf1));
    tm_buf1[0] = 0xCD; // prevent memcpy from being constant-folded into memset
    memcpy(tm_buf2, tm_buf1, sizeof(tm_buf1));
    memcpy(non_tm_buf, tm_buf1, sizeof(tm_buf1));
    memset(tm_int_buf, 0, sizeof(tm_int_buf));
}

int main() {
    tm_memops();
    return 0;
}
