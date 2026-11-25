# Developer Guide — TM API C++

## Project Overview

The TM API C++ project implements Software Transactional Memory (STM) across 26 C++ backends, supports two instrumentation pipelines (explicit API and LLVM plugin), has a Rust deterministic simulator with 7 simulation-enabled backends, and provides benchmark suites for evaluation.

## Directory Layout

```
├── backends/tm_impl/         # C++ TM backend implementations (26 backends)
│   ├── common/               # Shared: tm_hooks, tm_api, allocators, debug, trace
│   ├── tm_region_allocator/  # mmap-based TM region allocator
│   └── <backend>/            # Individual backends (tinystm, norec, tl2, swisstm, ...)
├── benchmarks/
│   ├── cpp/                  # Explicit API benchmarks (bank, STAMP, stmbench7, ...)
│   ├── plugin/               # Plugin benchmarks (LLVM-pass instrumented)
│   └── rust/                 # Rust benchmarks (Cargo workspace)
├── explicit_api/             # Explicit instrumentation (no LLVM plugin)
│   ├── cpp/expli_tm_api/     # tm_api.hpp (TM<T> wrapper); also cpp/include/ for older headers
│   └── rust/workspace/       # Rust API: tm crate + tm-executor + 17 runtime crates + containers + addrspace
├── plugin/                   # LLVM TM plugin (passes, runtimes, race checker)
│   ├── passes/               # 5-pass Honorio pipeline
│   ├── runtime/              # tm_runtime.cpp, persistent.cpp
│   └── bin/                  # libTMInstrument.so, libTMRaceChecker.so
├── simulator/                # Deterministic discrete-event simulator (Rust)
│   └── src/                  # Engine, model, backend abstraction, binaries
├── tests/                    # Test suites (explicit-api, plugin, backends)
├── docs/                     # Documentation
├── gpu/                      # GPU TM backends (CUDA + HIP) and GPU benchmarks
├── gem5_sim/                 # gem5-based HTM/TSX simulation
├── machine_profiles/         # CPU microarch profiles consumed by the simulator
├── patches/                  # Debug and profiling patches
└── .github/workflows/        # CI configuration (10 PR/push jobs + 7 nightly jobs)
```

## Build Systems

### Top-level Makefile

```
make                            # Print help (default target; same as `make help`)
make check-fast                 # ~60s smoke: plugin + 4 C++ backends + Rust (start here)
make all                        # Build plugin + plugin-benchmarks + expli-benchmarks
make plugin                     # Build libTMInstrument.so
make -C benchmarks/cpp all      # Build all expli benchmarks (BACKEND=tinystm default)
make -C plugin run              # Build and run plugin tests
make check-all                  # Build+test all C++ backends ($(BACKENDS_TESTS); SGL/LEFTRIGHT/ROMULUS skip auto-run)
make check-diff                 # Differential concurrency sweep: one self-verifying driver, every backend, same seeds
```

> **New here?** Run `make check-fast` to confirm the whole toolchain works, then
> read the [documentation index](README.md) to find the doc you need.

Backend selection: `BACKEND={TINYSTM,WBETL,WT,NOREC,NORECBF,SWISSTM,TL2,TSC_TM,MVLOG,SGL,LEFTRIGHT,ROMULUS,XTM,SPHT,TSXSGL,GPU_STM_CPU,CSMV,POWER8HTM,CALVIN,GACCO,GPUTX,JVSTM,ACCELTL2}` (23 values accepted by `benchmarks/cpp/Makefile`; `$(BACKENDS_TESTS)` in the top-level `Makefile` covers 21 of them for the auto-init sweep). Switching `BACKEND` forces a relink automatically (a stamp file records the last backend — see [correctness fixes §22](CORRECTNESS_FIXES.md)); before that fix, `make bin/test_tx BACKEND=B` after `BACKEND=A` could silently keep A's binary.

### Explicit API (no LLVM needed)

```
make -C benchmarks/cpp bin/test_tx BACKEND=NOREC
./benchmarks/cpp/bin/test_tx    # 120 tests
make -C benchmarks/cpp bin/test_ds BACKEND=NOREC
./benchmarks/cpp/bin/test_ds    # 207 tests
```

### LLVM Plugin (requires LLVM 22)

```
make plugin                     # Build libTMInstrument.so
make -C plugin race-checker     # Build libTMRaceChecker.so
make -C plugin test             # Build all 50+ plugin tests
make -C plugin run              # Run plugin tests
```

### Rust Workspace

```
cd explicit_api/rust/workspace
cargo check --features wbctl -p tm
cargo test --features wbctl -p tm
cargo test --features wbctl -p runtime-tinystm  # individual backend test (no sim)
cargo test --all-features -p runtime-romulus    # test with sim/serde/stats features
```

### Simulator

```
cd simulator
cargo run -- tm-sim --backend norec --trace /tmp/trace.jsonl
cargo run -- tm-gen --bank   # Generate synthetic traces
cargo test
```

### IDE / clangd

```
make compiledb                  # Generates compile_commands.json (requires bear or compiledb)
```

`make compiledb` rebuilds the plugin and one representative C++ backend
(`NOREC` `test_tx`/`test_ds`) under `bear` or `compiledb`, then merges the
captured commands into `compile_commands.json` for `clangd`.

## Two Instrumentation Pipelines

### Explicit API (Expli)

Manual annotation with `TM<T>` wrappers. No LLVM dependency. Ideal for development and testing.

```cpp
#include "expli_tm_api/tm_api.hpp"
expli::TM<int> counter;
expli::TM<int>::transaction([&] {
    counter.write(counter.read() + 1);
});
```

### LLVM Plugin (Plugin)

Automatic instrumentation via LLVM pass. Uses annotations on globals and functions.

```cpp
TM int counter;
TX void increment() { counter++; }
```

Pipeline: `collect → clone → redirect → instrument-fn → cleanup`

## How to Add a C++ Backend

1. Create `backends/tm_impl/<name>/<Name>_runtime.cpp` and `<name>.hpp`
2. Implement 23 static hook functions (begin, end, read/write for 7 types, alloc, env)
3. Build a `TMRealHooks` registration table with all function pointers
4. Register via `tm_register_real_hooks()` in `tm_init()`
5. Add `ifeq (BACKEND,<NAME>)` block in `benchmarks/cpp/Makefile`
6. Test: `make -C benchmarks/cpp bin/test_tx BACKEND=<NAME>`

## How to Add a Rust Backend

1. Create `explicit_api/rust/workspace/runtime/<name>/` with `Cargo.toml` + `src/lib.rs`
2. Export all `tm_read_*` / `tm_write_*` functions + `tm_begin`/`tm_commit`/`tm_abort`/lifecycle
3. Optionally add `pub mod sim` with simulation support
4. Register in `tm/Cargo.toml` and `tm/src/lib.rs`
5. Add exclusivity check in `tm/src/lib.rs`
6. Test: `cargo test --features <name> -p tm`

## Testing

| Test | Command | Count |
|------|---------|-------|
| Transaction correctness | `make -C benchmarks/cpp bin/test_tx BACKEND=NOREC` | 120 |
| Data structure correctness | `make -C benchmarks/cpp bin/test_ds BACKEND=NOREC` | 207 |
| All C++ backends | `make check-all` | `$(BACKENDS_TESTS)` (21) × 321 each (SGL/LEFTRIGHT/ROMULUS require manual `./bin/test_tx`) |
| Differential concurrency | `make check-diff` | 21 backends × 2 seeds × 2 thread counts, self-verifying |
| Plugin tests | `make -C plugin run` | ~25 instrumented + 2 race + 1 persist-sgl |
| Simulator lib tests | `cargo test --lib` | 60+ (7 sim backends × ~12 each) |
| Simulator integration | `cargo test --test sim_engine_test` | 26 |
| Rust workspace | `cargo test --manifest-path explicit_api/rust/workspace/Cargo.toml -- --test-threads=1` | 9 |

### Differential concurrency driver (`tests/concurrency/drv_diff_ops.cpp`)

`make check-diff` (or `tools/diff_backends.sh [seeds…]`, `BACKENDS="TL2 SGL"`
to narrow) builds one backend-agnostic driver per backend and runs it with
identical seeds. Every transactional write in the workload is a bounded
**additive delta**, so deltas commute and every legal commit order must
produce the same final state — the driver checks
`final(o) == init(o) + Σ deltas of committed txns to o` against a
post-commit shadow model, with no oracle binary and no history logging.
Extra in-transaction checks: repeatable reads (charged only on commit, so
legitimate OCC aborts don't count), reads within the window implied by the
concurrent commits, read-after-own-write (incl. the 1-byte path), and
transactional pointer slots that must always resolve to legal
`tm_malloc`'d targets. Waves of short-lived threads exercise TLS
lifecycle; a final single-threaded wave exercises `tm_malloc`/`tm_realloc`
grow/shrink/free after the slab has churned.

```
make -C benchmarks/cpp bin/drv_diff_ops BACKEND=TL2
./benchmarks/cpp/bin/drv_diff_ops -S 1 -T 8 -n 1500   # exit 2 = violation
  -S seed  -T threads  -n txns/thread
  -R   give each thread a disjoint object slice (no legitimate conflicts)
  -M mask op classes: 1 ADD, 2 TRANSFER, 4 CHECK, 8 READOWN, 16 slot
        double-read, 32 pointer churn, 64 multi-block batch, 128 alloc wave
  DIFF_OPS_DUMP=1 / _CHECK_EARLY=1 / _LOG=1 / _DIAG=1  per-object diffs,
        mid-run check, commit journal, layout + hook pointers
```

Read the file header before editing it: three setjmp/retry traps found
while building it (per-attempt resets and the shadow flush must both sit
*after* the `sigsetjmp` return point) make a *correct* backend look broken.

`tests/concurrency/mini_diff.cpp` (`bin/mini_diff`) is the stripped-down
core of the same idea — two threads, disjoint ranges, pure positive adds —
useful as a 20-line sanity check when porting a new backend or narrowing a
failure down to the API layer (`-shared` makes the ranges overlap).

## Debugging

Full playbook: **[DEBUGGING_BACKENDS.md](DEBUGGING_BACKENDS.md)** (event logger +
debug patches + Rust `tm-sim` trace replay, with a worked `bank_tl2` example).

- `DEBUG=1` — `-O0 -g` for all builds
- `-DTM_DEBUG_ALLOC` — track live TM allocations, detect leaks
- `-DTM_EVENT_LOG` — thread-local ring buffer with SIGSEGV auto-dump (`backends/tm_impl/common/tm_event_logger.hpp`); backends already emit `WRITE_SET_INSERT`/`READ_LOCK_ACQUIRE`/`COMMIT_*` events
- `TM_TRACE_PATH=/tmp/trc.jsonl` — emit JSONL traces for simulator replay
- `patches/debug/` — add back removed debug printfs without dirtying git: `./apply.sh` (whole set) / `git apply patches/debug/patches/NNN.patch` (single), `./remove.sh`, `./status.sh`; example `007-tl2-bypass-trace.patch` (count TL2 read/write-set vs `isTMAddress` bypass)
- `simulator/` — replay a trace through a real backend deterministically: `tm-gen`/`tm-sim`/`tm-check` (reproduce intermittent lost updates)
- `opt -passes="tm-race-checker"` — scan for missing TM annotations

## Key Patterns

- **Hooks**: All C++ backends register via `TMRealHooks` struct (23 function pointers). Hooks must be `static` to avoid TEXT-vs-DATA symbol conflicts.
- **Allocation**: `tm_region_malloc` allocates from a fixed mmap'd region. Inside transactions, `tm_track_spec_alloc` adds to the speculative allocation list (cleared on abort).
- **TLS**: Three shared TLS variables (`tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret`) defined in `tm_hooks.cpp`.
- **Simulation**: Rust backends gate thread-local state behind `#[cfg(feature = "simulation")]` — replaces `thread_local!` with `SyncUnsafeCell<HashMap<u64, State>>`. No `Mutex` needed since the simulator is single-threaded. Currently enabled for: norec, tl2, tinystm, romulus, swisstm, mvlog, tsc-tm, tsx_sim.
- **Checkpoint/Restore**: `sim_snapshot_bytes()` / `sim_restore_bytes()` serialize per-thread backend state via `bincode` for deterministic replay.
- **Adding sim to a new backend**: `pub TxState` (Clone + Serialize + Deserialize) + `#[cfg(feature = "simulation")]` variants of `with_tx`/`tx_active`/`flush_tx` + `pub mod sim` with 7 exported functions. See `explicit_api/rust/workspace/runtime/norec/src/lib.rs` for the canonical pattern.

## CI Pipeline

### `.github/workflows/ci.yml` (10 jobs, on `push` to main/master + `pull_request`)

| Job | Blocking? | What it does |
|-----|-----------|-------------|
| plugin-test | LLVM 22 yes / 23 no | Build plugin + race checker + run plugin tests (matrix over LLVM 22, 23) |
| simulator-test | yes | Build simulator + run all tests + synthetic trace fidelity check |
| rust-build | yes | Build Rust workspace + benchmarks + run workspace tests |
| race-checker | yes | Run `tm-race-checker` pass over every `.bc` produced by `make plugin` |
| cross-backend | yes (gated by event) | Build + test C++ backends (TINYSTM/NOREC/TL2 on PRs, 13 on push to main) |
| stale-check | yes | Assert legacy directories (`llvm_tm_plugin`, `expli-benchmarks`, `plugin-benchmarks`, `rust_tm_api`) are gone |
| tla-plus-check | no | Run `tlc2.TLC` over every `docs/proofs/*.tla` (with `|| true`) |
| tla-smoke | yes | Blocking 3-model smoke (`SGL`, `NOrec-smoke`, `TinySTM_WBCTL`), 60 s/model budget |
| lint | yes | `clang-format-22`, `cargo fmt --check`, `clippy -D warnings` |
| sanitizer | yes | ASan build + run of `bank`, `test_tx`, `test_ds` over NOREC/TL2 |

### `.github/workflows/nightly.yml` (7 jobs, cron)

| Job | Cadence | What it does |
|-----|---------|-------------|
| fidelity-regression | daily | `compare_real_sim.py` across 6 backends × 3 benchmarks; gate at 50% avg fidelity |
| cross-backend-full | daily | 15-backend CSV matrix (non-blocking) |
| post-merge-check | weekly (Sun) | `make post-merge-check` (full plugin + C++ + Rust + sim + integrity matrix) |
| audit | weekly | `cargo audit` on `simulator/` |
| tsan | weekly | ThreadSanitizer build + run of `bank`/`test_tx`/`test_ds` over NOREC/TL2 |
| coverage | weekly | `cargo llvm-cov` on the Rust workspace + simulator |
| gpu-compile-check | weekly | Compile GPU benchmarks on self-hosted runner; skips when no `nvcc`/`hipcc` |
