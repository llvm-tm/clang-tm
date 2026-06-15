/// Benchmark: inline pipeline — functions with `shared` annotation,
/// wrapped in manual tm_begin/tm_end retry loop in main().

#include <cstdio>
#include <chrono>

extern "C" {
void tm_begin(void);
int tm_end(int commit);
void tm_init(void);
void tm_init_thread(void);
void tm_exit_thread(void);
void tm_exit(void);
}

static int counter = 0;

__attribute__((noinline, annotate("shared")))
void inc() {
    counter++;
}

__attribute__((noinline, annotate("shared")))
void inc_heavy() {
    for (int j = 0; j < 100; j++)
        counter++;
}

int main() {
    tm_init();
    tm_init_thread();

    // --- Light TX: 1 inc per TX ---
    const int N = 50000;
    counter = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        tm_begin();
        inc();
        tm_end(1);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(end - start).count();
    printf("inline-light: %d tx in %.3f s = %.0f tx/s  counter=%d\n",
           N, sec, N / sec, counter);
    if (counter != N) return 1;

    // --- Heavy TX: 100 incs per TX ---
    const int N2 = 1000;
    counter = 0;
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N2; i++) {
        tm_begin();
        inc_heavy();
        tm_end(1);
    }
    end = std::chrono::high_resolution_clock::now();
    sec = std::chrono::duration<double>(end - start).count();
    printf("inline-heavy: %d tx (%d op) in %.3f s = %.0f tx/s  counter=%d\n",
           N2, N2 * 100, sec, N2 / sec, counter);
    if (counter != N2 * 100) return 1;

    printf("PASS\n");
    tm_exit_thread();
    tm_exit();
    return 0;
}
