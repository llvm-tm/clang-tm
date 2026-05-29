#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))

TM int64_t g_counter = 0;
TM int64_t g_array[4] = {0};

TX void tx_add(int64_t delta) {
    g_counter += delta;
}

TX void tx_write_array(int idx, int64_t val) {
    g_array[idx] = val;
}

TX int64_t tx_read_array(int idx) {
    return g_array[idx];
}

TX int64_t tx_read_counter() {
    return g_counter;
}

static void do_work(int iterations, int64_t delta, int n_threads) {
    if (n_threads <= 1) {
        for (int i = 0; i < iterations; i++) {
            tx_add(delta);
            tx_write_array(i % 4, tx_read_counter());
        }
    } else {
        std::vector<std::thread> threads;
        for (int t = 0; t < n_threads; t++) {
            threads.emplace_back([t, iterations, delta]() {
                for (int i = 0; i < iterations; i++) {
                    tx_add(delta);
                    tx_write_array(i % 4, (int64_t)(t * 1000 + i));
                }
            });
        }
        for (auto &th : threads) th.join();
    }
}

int main(int argc, char** argv) {
    const char *mode = "run";
    int iterations = 10;
    int64_t delta = 1;
    int n_threads = 1;
    bool crash = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            mode = argv[++i];
        else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            iterations = atoi(argv[++i]);
        else if (strcmp(argv[i], "--delta") == 0 && i + 1 < argc)
            delta = atol(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            n_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--crash") == 0)
            crash = true;
    }

    int64_t initial_counter = tx_read_counter();

    if (strcmp(mode, "init") == 0) {
        printf("MODE INIT: counter=%ld array=[%ld,%ld,%ld,%ld]\n",
               (long)tx_read_counter(),
               (long)tx_read_array(0), (long)tx_read_array(1),
               (long)tx_read_array(2), (long)tx_read_array(3));

    } else if (strcmp(mode, "run") == 0) {
        printf("MODE RUN: initial_counter=%ld delta=%ld iters=%d threads=%d\n",
               (long)initial_counter, (long)delta, iterations, n_threads);

        do_work(iterations, delta, n_threads);

        printf("  final_counter=%ld array=[%ld,%ld,%ld,%ld]\n",
               (long)tx_read_counter(),
               (long)tx_read_array(0), (long)tx_read_array(1),
               (long)tx_read_array(2), (long)tx_read_array(3));

        if (crash) {
            fprintf(stderr, "CRASH: simulating SIGKILL before clean exit\n");
            _exit(137);
        }

    } else if (strcmp(mode, "verify") == 0) {
        // Expected value is the last positional argument (all flag pairs consumed by the loop)
        int64_t expected = (argc > 3) ? atol(argv[argc - 1]) : 0;
        int64_t got = tx_read_counter();
        bool ok = (got == expected);
        printf("MODE VERIFY: expected=%ld got=%ld array=[%ld,%ld,%ld,%ld] %s\n",
               (long)expected, (long)got,
               (long)tx_read_array(0), (long)tx_read_array(1),
               (long)tx_read_array(2), (long)tx_read_array(3),
               ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }

    return 0;
}
