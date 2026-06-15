# TM API — Transactional Memory Framework

Multi-paradigm transactional memory framework for C++ (LLVM plugin, explicit C++ API, Rust bindings).
Supports 12+ STM/HTM backends.

## Structure

```
backends/          — TM runtime implementations (TinySTM, NOrec, TL2, SGL, SwissTM, etc.)
plugin/            — LLVM instrumentation plugin (5-pass Honorio pipeline)
benchmarks/        — Benchmarks (plugin-instrumented, C++ explicit, Rust)
tests/             — Tests for all components
expli_instr/       — Explicit C++ API headers + Rust workspace
tools/             — Build/install scripts
simulator/         — Rust discrete event simulator for TM trace replay
```

## Quick Start — Explicit C++ API (no LLVM plugin needed)

```bash
# Build and run with TinySTM (default)
make -C benchmarks/cpp -j4 run-test-tx

# Select a different backend
make -C benchmarks/cpp -j4 BACKEND=NOREC bin/test_tx
./benchmarks/cpp/bin/test_tx

# Run a benchmark
make -C benchmarks/cpp -j4 BACKEND=TINYSTM bin/bank
./benchmarks/cpp/bin/bank -t 2 -d 1000 --test
```

Available backends: `TINYSTM`, `WBETL`, `WT`, `NOREC`, `SWISSTM`, `TL2`, `SGL`, `LEFTRIGHT`, `ROMULUS`, `XTM`, `SPHT`, `TSXSGL`.

## Quick Start — LLVM Plugin

```bash
# 1. Build the plugin
make plugin

# 2. Build a plugin-instrumented benchmark
cd benchmarks/plugin/bank
make bank_singlelock
./bin/bank_singlelock -t 4 -d 5000
```

## Quick Start — Rust API

```bash
cd benchmarks/rust

# Build and run bank with NOrec
cargo run --release --no-default-features --features tm/norec --bin bank -- -d 100 -t 2 --test

# With default TinySTM backend
cargo run --release --bin bank -- -d 100 -t 2 --test
```

## Build and Run All Tests

```bash
# Explicit C++ API across all 12 backends
make check-all

# Or use the smoke test
./smoke_test.sh
```

## All Backend-Specific Plugin Benchmarks

| Backend       | Define                | Notes                        |
|---------------|-----------------------|------------------------------|
| TinySTM/WBCTL | `DESIGN_WBCTL`        | Write-back commit-time lock  |
| TinySTM/WBETL | `DESIGN_WBETL`        | Write-back encounter-time    |
| TinySTM/WT    | `DESIGN_WT`           | Write-through + undo log     |
| NOrec         | —                     | Lazy value-based validation  |
| TL2           | —                     | Commit-time locking          |
| SwissTM       | —                     | Hybrid lazy/pessimistic      |
| SingleLock    | —                     | Serial execution             |
| LeftRight     | —                     | Concurrent read, serialized  |
| Romulus       | —                     | Redo logging                 |
| XTM           | —                     | Experimental                 |
| SPHT          | `-mrtm`               | RTM + epoch commit log       |
| TSXSGL        | `-mrtm`               | TSX + single global lock     |
| DUDETM        | `DESIGN_WBCTL`        | Commit + redo log (plugin)   |
| NVHTM         | `-mrtm`               | RTM + NVM (plugin)           |

## Plugin Race Checker

```sh
opt-22 -load-pass-plugin=plugin/bin/libTMRaceChecker.so \
       -passes="tm-race-checker" myapp.bc -o /dev/null
```

## Quick Examples

### 1. LLVM Plugin Path

Annotate TM globals with `TM` and transaction functions with `[[tm::shared]]`:

```cpp
#include <cstdio>

// The TM attribute marks global variables and locals as TM-tracked.
// The LLVM pass instruments all accesses to them inside [[tm::shared]] functions.
#define TM __attribute__((annotate("tm")))

TM int32_t counter = 0;           // TM-shared global

// [[tm::shared]] functions: loads/stores to TM globals are
// automatically replaced with tm_read_i4/tm_write_i4 calls.
__attribute__((annotate("shared")))
void increment(int n) {
    for (int i = 0; i < n; i++)
        counter = counter + 1;
}

__attribute__((annotate("thread")))
void worker() {
    increment(1000);
}

int main() {
    increment(1000);
    printf("counter = %d\n", (int)counter);    // 2000
    return 0;
}
```

Compile with `clang-tm`:
```sh
clang-tm --runtime=plugin/runtime/tm_runtime.cpp -o myapp myapp.cpp
./myapp
```

See `tests/plugin/test_types.cpp` for more type-specific examples.
The full compilation pipeline is documented in `plugin/README.md`.

### 2. Explicit C++ API (no compiler plugin)

Use `expli::TM<T>::transaction()` — compiles with any C++20 compiler:

```cpp
#include <cstdio>
#include "tm_api.hpp"

// TM-tracked struct (allocated on regular heap, fields are TM)
struct Account {
    expli::TM<int64_t> balance;
};

int main() {
    Account acc;
    acc.balance.poke(100);               // non-TM write (init)

    // transaction() wraps tm_begin/tm_end with retry loop
    expli::TM<int64_t>::transaction([&]() {
        int64_t v = acc.balance.read();  // tm_read_i8
        acc.balance.write(v + 50);       // tm_write_i8
    });

    printf("balance = %lld\n", (long long)acc.balance.peek());  // 150
    return 0;
}
```

Build + run with TinySTM (see `backends/README.md` for all backends):
```sh
make -C benchmarks/cpp BACKEND=TINYSTM run-tests
# The test suite includes test_tx which exercises this pattern.
```

See `tests/expli-api/test_tx.cpp` for complete unit tests
(`./bin/test_tx` after `make -C benchmarks/cpp all`).
A money-conservation benchmark is at `benchmarks/cpp/bank/bank.cpp`.

### 3. Rust API (no compiler plugin)

```rust
use tm::transaction;

fn main() {
    let balance = TmCell::new(100i64);
    transaction(|tx| {
        let v = balance.load(tx);
        balance.store(tx, v + 50);
    });
    println!("balance = {}", balance.load(&tm::GlobalTx::new()));
}
```

Run with the default backend (TinySTM/WBCTL):
```sh
cargo run --bin myapp
```

See `expli_instr/rust/workspace/README.md` for backend selection and benchmark details.

---

## Known Issues

| Issue | Details |
|-------|---------|
| **LeftRight test_tx fails** | 29/114 tests fail (pre-existing algorithm bug, not related to build fixes) |
| **Romulus test_tx fails** | 54/114 tests fail (pre-existing algorithm bug, not related to build fixes) |
| **DUDETM, NVHTM, DistributedSGL, PersistentSGL** | Build but depend on plugin-provided symbols (`tm_symbol_count`, `tm_symbol_addresses`, `tm_symbol_sizes`) not available in explicit C++ API |
| **rbtree benchmark timing** | Reports 0ms duration (pre-existing) |
| **stmbench7 times out** | Data race in `ts_multimap::lower_bound()` (pre-existing) |
| **Plugin pipeline tests** | Need `clang-tm` wrapper testing (auto-link of `tm_hooks.cpp` fix not fully verified) |
