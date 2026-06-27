// ── Wrapper for live-app simulation ─────────────────────────────
// Provides an extern "C" entry point that tm-live can look up.
extern "C" int run_benchmark(int argc, char *argv[]);

extern "C" int run_benchmark(int argc, char *argv[]) {
    extern int main(int, char**);
    return main(argc, argv);
}
