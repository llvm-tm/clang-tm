# TM API — Transactional Memory Framework

Multi-paradigm transactional memory framework for C++ (LLVM plugin, explicit C++ API, Rust bindings).
Supports 12+ STM/HTM backends.

## Structure

```
backends/          — TM runtime implementations (TinySTM, NOrec, TL2, SGL, SwissTM, etc.)
plugin/            — LLVM instrumentation plugin (5-pass Honorio pipeline)
benchmarks/        — Benchmarks (plugin-instrumented, C++ explicit, Rust)
tests/             — Tests for all components
explicit_api/      — Explicit C++ API headers + Rust workspace
tools/             — Build/install scripts
simulator/         — Rust discrete event simulator for TM trace replay
gpu/               — GPU TM backends (CUDA + HIP) and GPU benchmarks
gem5_sim/          — gem5-based HTM/TSX simulation (setup.sh clones upstream gem5)
machine_profiles/  — CPU microarchitecture profiles consumed by the simulator
patches/           — Debug printfs + TSX timing instrumentation patches
docs/              — Documentation index, audits, proofs (TLA+), and the book
```

The full documentation index lives in [`docs/README.md`](docs/README.md);
session history in [`CHANGELOG.md`](CHANGELOG.md); open work in
[`TODO.md`](TODO.md).

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

> **Note (Linux):** `benchmarks/cpp` links statically by default (self-contained
> binaries). Pass `STATIC=0` to link dynamically instead (faster builds):
> `make -C benchmarks/cpp BACKEND=NOREC STATIC=0 bin/bank`.

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

## Verify Your Setup

```bash
# ~60s smoke test: plugin + 18 instrumented plugin tests +
# test_tx/test_ds for TINYSTM/NOREC/TL2 + Rust simulator + Rust workspace.
# Stops at the first failure.
make check-fast

# Full sweep: test_tx/test_ds across all C++ backends (slow).
make check-all
```

`make` with no arguments prints a summary of all top-level targets
(`make help`).

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

See `tests/explicit-api/test_tx.cpp` for complete unit tests
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

See `explicit_api/rust/workspace/README.md` for backend selection and benchmark details.

---

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for how to add a backend (6-step
C++/Rust walkthrough), run the test suite, commit style, and the PR checklist.
Verify your toolchain with `make check-fast` and formatting with
`make fmt-check`. Security reports are handled in [`SECURITY.md`](SECURITY.md);
`.github/CODEOWNERS` marks the plugin/common backend/build paths as
maintainer-owned.

---

## Known Issues

> The full, current list of open work items lives in [`TODO.md`](TODO.md).
> This table highlights the backend-specific gotchas.

| Issue | Details |
|-------|---------|
| **JVSTM `.peek()` reads** | Direct/`.peek()` reads on a `TM<T>` use a plain memory load. For most backends this is correct, but for JVSTM (where values live in a `VBox` linked list and are not written back to the original address) it can miss committed values. `test_tx`/`test_ds` pass for JVSTM as of 2026-09-21; if you write new code that calls `.peek()` on a JVSTM-backed `TM<T>`, prefer the `TM<T>::load()` accessor instead. |
| **Calvin `test_tx` segfault** | After two read-after-write failures, `test_tx` segfaults (pre-existing). `check-all` skips Calvin's `test_tx` (but still runs `test_ds`, which passes 207/207). See `review-04/BUGS.md` (not shipped). |
| **Calvin multi-thread `bank`** | `bank` conserves money at 1T; 4T can destroy money under high abort rates in the execute phase (pre-existing two-phase OCC contention issue). |
| **TinySTM `proactive_stop` workaround** | RESOLVED (2026-09-25): all 15 `proactive_stop` sites removed. Root causes were the quadratic vector write-set lookups in long transactions, unbounded plugin retry loops, and a deferred-free publication race — see `docs/CORRECTNESS_FIXES.md` §16. |
| **TL2 plugin-mode bank (`bank_tl2 --test`)** | Reports small money drift (`Got: 1024002` vs expected `1024000`). Surfaced after the B-01 compile fix; other plugin modes (`singlelock`, `norec`, `tinystm`) conserve money. See `review-04/BUGS.md` (not shipped) B-21. |
| **`ycsb` plugin-mode `init_record`** | `snprintf` writes through a `TM Record*` arg, bypassing `tm_write_i1`. The build succeeds (the Makefile sets `-tm-allow-opaque`), but the concurrent write is not tracked by any STM. See `review-04/BUGS.md` (not shipped) B-25. |
| **`STAMP` plugin-mode `stamp_*_plugin_tinystm`** | Link fails with `undefined reference to 'tm_init.1'` etc. — the clone pass emits cloned symbols in the benchmark bitcode but `--link-only` mode does not emit matching definitions in the runtime bitcode. See `review-04/BUGS.md` (not shipped) B-27. |
| **`stmbench7` plugin-mode link** | Uninstrumented link missing `tm_set_num_threads`, `tinystm::g_tm_stop_requested`, `tm_get_thread_state`, `tm_get_env`, `tm_set_jmpbuf`, etc. See `review-04/BUGS.md` (not shipped) B-07 / B-29. |
| **stmbench7 runtime hang** | Data race in `ts_multimap::lower_bound()` causes hangs under TinySTM / TL2 (pre-existing). |
| **DUDETM, NVHTM, DistributedSGL, PersistentSGL** | Build, but depend on plugin-provided symbols (`tm_symbol_count`, `tm_symbol_addresses`, `tm_symbol_sizes`) not available in the explicit C++ API. |
| **Plugin pipeline tests** | Need `clang-tm` wrapper testing (auto-link of `tm_hooks.cpp` fix not fully verified). |
| **gem5 Ruby `MESI_Three_Level` livelock** | gem5 HTM multi-thread livelock under `MESI_Three_Level`; port to `MESI_Two_Level` is open work. See `review-04/BUGS.md` (not shipped) B-15. |
| **GPU benchmark stubs (`gpu_tpcc`, `gpu_memcached`, `gpu_kmeans`)** | Compile and run but are not real implementations; they exercise the API surface only. Either promote to real implementations or delete. See `review-04/BUGS.md` (not shipped) B-16. |
| **TSC-TM / CSMV simulator coverage** | The Rust deterministic simulator does not yet have TSC-TM or CSMV backends; see `review-04/BUGS.md` (not shipped) B-17. |
| **`tests/plugin/regression/old_code/perf.c`** | Unfinished measurements; either complete or delete. See `review-04/BUGS.md` (not shipped) B-18. |
