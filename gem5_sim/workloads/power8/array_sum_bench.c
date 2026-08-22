static inline long
sys_exit(int code)
{
    register long r3 asm("3") = code;
    register long r0 asm("0") = 1;
    asm volatile("sc" : : "r"(r3), "r"(r0) : "memory");
    return r3;
}

static inline long
sys_write(int fd, const void *buf, unsigned long count)
{
    register long r3 asm("3") = fd;
    register long r4 asm("4") = (long)buf;
    register long r5 asm("5") = count;
    register long r0 asm("0") = 4;
    asm volatile("sc" : "=r"(r3) : "r"(r3), "r"(r4), "r"(r5), "r"(r0) : "memory");
    return r3;
}

static long
strlen(const char *s)
{
    long n = 0;
    while (*s++) n++;
    return n;
}

static void
print_str(const char *s)
{
    long len = strlen(s);
    sys_write(1, s, len);
}

static void
print_dec(long val)
{
    char buf[24];
    long i = sizeof(buf) - 1;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    buf[i] = '\n';
    i--;
    if (val == 0) {
        buf[i] = '0';
        i--;
    } else {
        while (val > 0 && i >= 0) {
            buf[i] = '0' + (val % 10);
            val /= 10;
            i--;
        }
    }
    if (neg) { buf[i] = '-'; i--; }
    sys_write(1, buf + i + 1, sizeof(buf) - i - 1);
}

/* 256-element array initialized at compile time */
static const long data[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
};

void
_start(void)
{
    long sum = 0;
    long i;
    int tx_failed = 0;

    asm volatile(
        "tbegin. ;"
        "beq+ 1f ;"
        "li %0, 1 ;"
        "b 2f ;"
        "1: li %0, 0 ;"
        "2: ;"
        : "=r"(tx_failed)
        :
        : "cr0"
    );

    if (tx_failed) {
        print_str("transaction failed\n");
        sys_exit(1);
    }

    for (i = 0; i < 256; i++) {
        sum += data[i];
    }

    asm volatile(
        "tend."
        : : : "cr0"
    );

    print_str("sum = ");
    print_dec(sum);

    sys_exit(0);
}
