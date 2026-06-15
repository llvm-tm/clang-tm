# Benchmark Self-Tests & Debugging Methodology

## Quick Start

Run all benchmark self-tests:

```bash
cd benchmarks/cpp
make          # build all benchmarks
./tests/run_benchmark_tests.sh
```

Test a single benchmark:

```bash
./bin/<benchmark> --test
```

## Test Coverage

Each benchmark has a `--test` flag that runs self-tests. The tests cover:

| Category | What's Tested |
|----------|---------------|
| **CLI flags** | Default values correct, parsing overrides defaults, help flag works |
| **RNG determinism** | Same seed → same 1000-value sequence (1000 assertions) |
| **Core logic** | Geometry (yada), clustering (kmeans), manager ops (vacation), segment gen (genome), packet gen (intruder), path routing (labyrinth), scoring (bayes), graph CSR (ssca2) |

## Result: 15 benchmarks × ~1015 assertions each = ~15K assertions, 0 failures

## Debugging Methodology

### 1. Self-Test Mode (`--test`)

Every benchmark has a `--test` flag that runs unit tests independent of the TM runtime:

```bash
./bin/yada --test
  PASS (4 assertions)
  PASS (1000 assertions)
  PASS (7 assertions)
```

The test mode:
- Does NOT require TM region initialization (for logic/RNG tests)
- Tests CLI parsing by constructing argv arrays internally
- Tests RNG determinism by comparing two generators with the same seed
- Tests core geometry/algorithms with known-correct reference values

### 2. Debug Build with GDB

Build debug symbols (`-O0 -g`) and run under GDB:

```bash
g++ -g -O0 ... -o bin/yada_debug STAMP/yada/yada.cpp ...
gdb --args ./bin/yada_debug -a 20 -j 0.5 -t 2
(gdb) run
(gdb) bt          # backtrace on crash
(gdb) info locals # inspect local variables
```

For multi-threaded crashes:
```bash
(gdb) thread apply all bt
```

### 3. Sanitizers

Use AddressSanitizer or UndefinedBehaviorSanitizer:

```bash
g++ -fsanitize=address -g -O1 ... -o bin/yada_asan ...
./bin/yada_asan --test
```

```bash
g++ -fsanitize=undefined -g -O1 ... -o bin/yada_ubsan ...
./bin/yada_ubsan --test
```

### 4. Deterministic Stress Testing with Fuzz Benchmarks

The `fuzz_counter` and `fuzz_bank` benchmarks are designed for stress-testing the TM system under controlled conditions:

```bash
# Counter fuzz: N threads × K increments on M counters
./bin/fuzz_counter <threads> <iters> <counters> <seed>
  e.g., ./bin/fuzz_counter 4 10000 8 42

# Bank fuzz: concurrent transfers between accounts
./bin/fuzz_bank <threads> <accounts> <transfers> <seed>
  e.g., ./bin/fuzz_bank 4 100 5000 42
```

These benchmarks:
- Accept a seed for deterministic reproduction
- Verify invariants (sum of all counters = initial + committed; all transfers balance)
- Exercise the TM system with fine-grained concurrent operations
- Report abort statistics for performance analysis

To systematically fuzz a TM backend:

```bash
# Vary thread count and workload
for t in 1 2 4 8 16; do
    for i in 100 1000 10000; do
        ./bin/fuzz_counter $t $i 8 42
    done
done
```

### 5. Invariant Verification

Every benchmark has a built-in result checker:

| Benchmark | Invariant |
|-----------|-----------|
| yada | `PASS` printed at end (all triangles satisfy angle constraint) |
| kmeans | Converges (delta < threshold) |
| vacation | Total ops match expected count |
| labyrinth | `Verification passed.` (all paths are valid) |
| intruder | `Num found` matches expected |
| genome | Unique segments printed |
| bayes | `PASS` (runs without error) |
| fuzz_counter | Counter sum matches expected (internal check) |
| fuzz_bank | All transfers balance (internal check) |

### 6. Workflow for Finding and Fixing Bugs

```bash
# 1. Run self-tests first (catches parsing/RNG/logic errors)
./bin/<benchmark> --test

# 2. Run benchmark with small workload (fast iteration)
./bin/<benchmark> -p 2 -n 100

# 3. If crash, reproduce under GDB
gdb --args ./bin/<benchmark> -p 2 -n 100
(gdb) run
(gdb) bt

# 4. If data corruption, use AddressSanitizer
./bin/<benchmark>_asan -p 2 -n 100

# 5. Isolate TM vs non-TM issue: run same workload with
#    SingleGlobalLock backend (simplest, no concurrency)
#    or SingleGlobalLock under GDB

# 6. For multi-threaded heisenbugs, use fuzz benchmarks
#    with varying thread counts to increase reproducibility
./bin/fuzz_counter 56 10000 64 42  # high contention
```

### 7. Parameter Verification

To verify that CLI parameters actually affect the workload:

```bash
# Run with two different sizes and compare output
./bin/genome -p 2 -g 1000 -s 50 -n 200 | grep "Unique segments"
./bin/genome -p 2 -g 500 -s 30 -n 100 | grep "Unique segments"
# Different parameters → different results
```

## Adding Tests to a New Benchmark

1. Include the test header from `benchmarks/cpp/tests/`:
   ```cpp
   #include "../tests/benchmark_test.hpp"
   ```

2. Refactor arg parsing into a function:
   ```cpp
   static void parse_args(int argc, char* argv[]) {
       // ... existing parsing logic ...
       if (strcmp(argv[1], "--test") == 0) {
           exit(run_all_tests() ? 1 : 0);
       }
   }
   ```

3. Write test functions:
   ```cpp
   static int test_cli_flags() { /* ... */ return test_result(); }
   static int test_rng() { /* ... */ return test_result(); }
   static int test_logic() { /* ... */ return test_result(); }

   static int run_all_tests() {
       int fails = 0;
       fails += test_cli_flags();
       fails += test_rng();
       fails += test_logic();
       return fails;
   }
   ```

4. Use `TEST_ASSERT(cond, msg)`, `TEST_EQ(a, b, msg)`, `TEST_NEAR(a, b, eps, msg)`.

## Adding a New Fuzz Benchmark

1. Create `benchmarks/cpp/<name>/<name>.cpp`
2. Include TM wrappers and declare a deterministic RNG
3. Define an invariant that must hold after concurrent TM operations
4. Add to `benchmarks/cpp/Makefile`
5. Add `--test` support
