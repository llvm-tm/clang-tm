/**
 * Bare-metal POWER8 HTM test — no libc, no startup code.
 * Write syscall is used directly for all output.
 */
#define SYS_write 1
#define SYS_exit 2

static inline long syscall3(long n, long a, long b, long c) {
    long ret;
    register long r0 __asm__("r0") = n;
    register long r3 __asm__("r3") = a;
    register long r4 __asm__("r4") = b;
    register long r5 __asm__("r5") = c;
    __asm__ __volatile__("sc" : "=r"(r0), "=r"(r3), "=r"(r4), "=r"(r5)
                         : "r"(r0), "r"(r3), "r"(r4), "r"(r5)
                         : "r6", "r7", "r8", "r9", "r10", "r11", "r12",
                          "cr0", "memory");
    (void)r0; (void)r4; (void)r5;
    return r3;
}

static inline long syscall1(long n, long a) {
    return syscall3(n, a, 0, 0);
}

static long my_strlen(const char *s) {
    long len = 0;
    while (*s++) len++;
    return len;
}

static void print_str(const char *s) {
    syscall3(SYS_write, 1, (long)s, my_strlen(s));
}

static void print_hex(unsigned long long v) {
    char buf[19];  /* "0x" + 16 hex digits + null */
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        int digit = (v >> (60 - 4*i)) & 0xf;
        buf[2 + i] = digit < 10 ? '0' + digit : 'a' + digit - 10;
    }
    buf[18] = '\n';
    syscall3(SYS_write, 1, (long)buf, 19);
}

/* POWER8 HTM helpers */
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

void _start(void) {
    unsigned long long counter = 0;
    int commits = 0, aborts = 0;

    print_str("HTM Test: 5 iterations\n");

    for (int i = 0; i < 5; i++) {
        if (tbegin()) {
            unsigned long long v = counter;
            v += 1;
            counter = v;
            tend();
            commits++;
        } else {
            aborts++;
        }
    }

    print_str("commits: ");
    print_hex(commits);
    print_str("aborts:  ");
    print_hex(aborts);
    print_str("counter: ");
    print_hex(counter);

    if (counter == 5) {
        print_str("PASS\n");
        syscall1(SYS_exit, 0);
    } else {
        print_str("FAIL: expected 5\n");
        syscall1(SYS_exit, 1);
    }

    /* unreachable */
    for (;;);
}
