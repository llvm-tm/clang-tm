# Improvement Plan

## Current State

The TM API C++ project has 3 instrumentation pipelines (standard, inline, queue), 16 STM/HTM backends (C++ + Rust), a Rust discrete-event simulator, an LLVM race checker pass, and 21 documentation files. Tests pass across 10 backends (114/114 test_tx, 207/207 test_ds). The simulator supports 3 backends (NOrec, TL2, TinySTM) for trace replay.

## Tier 1 — Bugs

### 1.1 Fix remaining spin loops in simulation mode

**Root cause**: Only `validate_impl` in NOrec was fixed to return `None` when `GLOBAL_LOCK` is locked in simulation mode. There are 22+ remaining spin loops across the 3 simulator backends that will hang when a lock is held by a thread that never runs (sequential replay).

**Locations**:
- NOrec: `read_word` inner loop (norec/src/lib.rs:220), `tm_begin` (norec/src/lib.rs:374)
- TL2: read-lock acquisition (tl2/src/lib.rs:188), commit lock paths (tl2/src/lib.rs:277, 299)
- TinySTM: 16 spin loops across wbctl/wbetl/wt/common/raw

**Fix pattern**: Add `#[cfg(feature = "simulation")]` guard before each `std::hint::spin_loop()` to either abort (tx commit path) or proceed (if lock is only needed for torn-read protection, skip the spin).

**Verification**: Run `cargo test -- --test-threads=1` (75 tests). Run `tm-sim` on multi-threaded bank traces that previously hung.

### 1.2 Integrate deadlock detector into TL2 and TinySTM sim paths

**Root cause**: `SimEngine` wires the `DeadlockDetector` only for NOrec (in `dispatch_event` TxEnd path). TL2 and TinySTM trigger `record_commit`/`record_abort` only via the generic backend path.

**Fix**: Move deadlock recording into `Backend::commit()` and `Backend::abort()` in `backend.rs` so all 3 backends are covered uniformly.

### 1.3 Investigate LEFTRIGHT 29/114 test_tx failures

**Root cause**: Algorithm-level bug. Left-right barrier deadlocks with >1 thread. Detailed analysis in `backends/tm_impl/leftright/Implementation_notes.md`.

**Fix**: Needs algorithmic redesign of barrier synchronization. Lower priority (no active work on this).

## Tier 2 — Infrastructure

### 2.1 Delete stale legacy directories

Remove `llvm_tm_plugin/`, `expli-benchmarks/`, `plugin-benchmarks/`, `rust_tm_api/` after confirming no active references. Check `Makefile`, `AGENTS.md`, `docs/` for stale path references and update.

### 2.2 Add simulation feature to more Rust backends

**Current**: Only NOrec, TL2, TinySTM have `#[cfg(feature = "simulation")]` thread-id multiplexing.

**Target**: Add `simulation` feature to SwissTM, ROMULUS, SGL (the most-used backends after the 3 current ones). Each needs:
- Replace `thread_local!` with `Mutex<HashMap<u64, State>>`
- Add `sim` module with `snapshot_states()`, `restore_states()`, `reset()`
- Register in `backend.rs`
- Add `sim_backend_<name>` feature flag in `simulator/Cargo.toml`

**Verification**: `cargo build --features sim-backend-swisstm` + synthetic trace replay matches expected commit/abort counts.

### 2.3 Add CI for simulator and Rust backends

**Current CI** (`.github/workflows/ci.yml`):
- Builds plugin + race checker
- Runs expli NOREC tests + plugin tests
- Verifies annotations

**Target CI** (6 jobs):

| Job | Triggers | What it does |
|-----|----------|-------------|
| `plugin-test` | PR + push | Existing smoke test |
| `race-checker` | PR + push | Run race checker on all benchmarks, count warnings |
| `simulator-test` | PR + push | `cargo test -- --test-threads=1` + synthetic trace fidelity check |
| `rust-build` | PR + push | Build all Rust workspace members |
| `cross-backend` | push to main | Build + run test_tx + test_ds on all 12 backends |
| `fidelity-regression` | nightly | compare_sim.py on synthetic + real traces, publish CSV |

### 2.4 Fix C++ ↔ simulator address mismatch

**Root cause**: `tm-sim` mmap's at `0x7f00_0000_0000`. Real C++ benchmarks use OS-chosen addresses from `mmap(nullptr, ...)`. When a C++ trace is loaded, the simulator tries to read from addresses that aren't in its mapped range → SIGBUS/crash.

**Approaches**:
- **(a) Hybrid**: `tm-sim` reads from allocated TM region via `Backend::read_word()`. If address is not in mapped range, fall back to `tm-check` (WBCTL model, no memory access). Already partially done in `compare_real_sim.py`.
- **(b) Region remap**: `init_from_events()` already scans trace events for address range. If the range is in the OS heap (not the fixed `0x7f00` address), mmap at those addresses. Works for most traces. **Recommended.**
- **(c) ld-preload shim**: Intercept C++ mmap calls in benchmarks to force addresses into the simulator's known range. More invasive but gives exact match.

**Recommended approach**: (b) — the address scanning already exists. The issue is that `init_from_events()` is only called from `tm-sim` but not from `compare_real_sim.py`'s sim runner. Fix: make `tm-sim` always auto-detect address range from trace events.

## Tier 3 — Architecture

### 3.1 Concurrent simulation engine

**Problem**: The simulator processes events sequentially. No true concurrency means CAS always succeeds, lock contention never happens, and TM quality assessment is limited.

**Design sketch**: Spawn N OS threads, each managing one simulated thread's event queue. Use a shared barrier at commit points and a scheduler that can explore interleavings.

**Scope**: 1-2 person-weeks. Needs careful design to avoid introducing non-determinism.

**Status**: Design phase. No active work until Tiers 1-2 are resolved.

## Tier 4 — Polish

### 4.1 Add developer onboarding guide

Create `docs/DEVELOPER_GUIDE.md` covering:
- Project structure overview (component map)
- How to add a new backend (C++ + Rust)
- How the pipeline variants work
- Build and test workflow
- Debugging tips (strace, GDB, Helgrind)

### 4.2 Rust backend feature exclusivity

Add `compile_error!` or `#[cfg(all(...))]` check in `tm/src/lib.rs` to reject selecting more than one backend feature.

---

## CI/CD Pipeline

### Jobs

```yaml
name: CI

on:
  push:
    branches: [main, master]
  pull_request:
    branches: [main, master]

jobs:
  # ── Job 1: Plugin smoke test (existing, extended) ──────
  plugin-test:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install LLVM 22
        run: install llvm-22-dev clang-22 lld make
      - name: Build plugin + tests + race checker
        run: make plugin && make -C plugin race-checker && make -C plugin test
      - name: Run plugin tests
        run: make -C plugin run
      - name: Verify annotations
        run: make -C plugin check

  # ── Job 2: Simulator tests ─────────────────────────────
  simulator-test:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install Rust
        uses: dtolnay/rust-toolchain@stable
      - name: Build simulator
        run: cargo build --manifest-path simulator/Cargo.toml
      - name: Run simulator unit tests
        run: cargo test --manifest-path simulator/Cargo.toml -- --test-threads=1
      - name: Synthetic trace fidelity check
        run: |
          python3 simulator/compare_sim.py --sim-only --backends norec,tl2,tinystm 2>&1

  # ── Job 3: Rust build + smoke test ─────────────────────
  rust-build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install Rust
        uses: dtolnay/rust-toolchain@stable
      - name: Build all Rust workspace members
        run: cargo build --manifest-path explicit_api/rust/workspace/Cargo.toml
      - name: Build all Rust benchmarks
        run: cargo build --manifest-path benchmarks/rust/Cargo.toml
      - name: Run Rust tests
        run: cargo test --manifest-path explicit_api/rust/workspace/Cargo.toml

  # ── Job 4: Cross-backend correctness (push to main only) ─
  cross-backend:
    if: github.event_name == 'push'
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install LLVM 22
        run: install llvm-22-dev clang-22 lld make
      - name: Build + test all 12 backends
        run: |
          for backend in TINYSTM WBETL WT NOREC SWISSTM TL2 SGL XTM LEFTRIGHT ROMULUS; do
            make -C benchmarks/cpp -j4 bin/test_tx bin/test_ds BACKEND=$backend
            echo "=== $backend ==="
            ./benchmarks/cpp/bin/test_tx && echo "  test_tx: PASS" || echo "  test_tx: FAIL"
            ./benchmarks/cpp/bin/test_ds && echo "  test_ds: PASS" || echo "  test_ds: FAIL"
          done

  # ── Job 5: Race checker ────────────────────────────────
  race-checker:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install LLVM 22
        run: install llvm-22-dev clang-22 lld make
      - name: Build race checker
        run: make -C plugin race-checker
      - name: Run race checker on plugin benchmarks
        run: |
          for bc in plugin/bin/*.bc; do
            opt-22 -load-pass-plugin=plugin/bin/libTMRaceChecker.so \
              -passes="tm-race-checker" "$bc" -o /dev/null 2>&1 | \
              grep -v "false positive" || true
          done
```

### CD / Nightly

```yaml
name: Nightly

on:
  schedule:
    - cron: '0 6 * * *'

jobs:
  fidelity-regression:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install dependencies
        run: sudo apt-get install llvm-22-dev clang-22 lld make
      - name: Build simulator
        run: cargo build --release --manifest-path simulator/Cargo.toml
      - name: Build C++ benchmarks + run comparison
        run: python3 simulator/compare_real_sim.py --csv nightly_results.csv
      - name: Upload results
        uses: actions/upload-artifact@v4
        with:
          name: fidelity-results
          path: nightly_results.csv
```

---

## Prioritization

| Priority | Item | Effort | Impact | Depends on |
|----------|------|--------|--------|------------|
| P0 | 1.1 Fix remaining spin loops | 1-2h | Critical | — |
| P0 | 2.3 Add CI jobs | 2-3h | Critical | 2.1 |
| P1 | 1.2 Integrate deadlock detector into TL2/TinySTM | 1h | High | 1.1 |
| P1 | 2.1 Delete stale directories | 30min | Medium | — |
| P1 | 2.4 Fix C++ ↔ simulator address mismatch | 2h | High | 1.1 |
| P2 | 2.2 Add simulation feature to SwissTM/ROMULUS/SGL | 4h | High | 1.1, 2.4 |
| P3 | 4.1 Developer onboarding guide | 2h | Medium | 2.1 |
| P3 | 4.2 Rust backend feature exclusivity | 30min | Low | — |
| P4 | 3.1 Concurrent simulation engine | 1-2 weeks | Very High | P0-P2 |
| P4 | 1.3 LEFTRIGHT 29/114 fix | Unknown | Medium | — |

## Tier 5 — GPU STM Backends

### 5.1 First backend: PR-STM (priority-based lock STM)

**Goal**: Implement publishable GPU STM algorithm as a TMRealHooks backend.

**Algorithm**: PR-STM (Shen et al., 2015) — lock-based, commit-time validation
with static thread priorities for deadlock-free lock stealing. 32-bit lock word
(priority + version + locked flag) is GPU-friendly (single atomicCAS per lock).

**Architecture**: CUDA C++ library + C API matching TMRealHooks.

```
backends/tm_impl/gpu_stm/
  CMakeLists.txt           -- CUDA-enabled build
  include/gpu_stm_api.h    -- C API (TMRealHooks-compatible)
  cpu/
    gpu_stm_cpu_runtime.cpp -- CPU fallback runtime
    pr_stm_cpu.cpp          -- CPU emulation (std::thread as lanes)
  cuda/
    pr_stm_kernel.cuh      -- CUDA kernel: warp-level PR-STM
    pr_stm_runtime.cu      -- CUDA host runtime, kernel launch
  tests/
    test_pr_stm.cpp        -- Smoke test (CPU + CUDA)
```

**Status**: **CPU fallback done** (114/114 test_tx, 207/207 test_ds).
CUDA kernel template exists (`pr_stm_kernel.cuh`) but not built by CI.
TLA+ model at `docs/proofs/GPU_PR_STM.tla`.

### 5.2 Second backend: CSMV (multi-versioned client-server)

**Goal**: Add CSMV backend (Nunes et al., IPDSS 2022) — multi-versioned STM
with client-server commit protocol. Server warp validates, clients batch writes.
K-version read sets allow read-only transactions to commit without validation.

**Status**: **CPU fallback done** (114/114 test_tx, 207/207 test_ds).
CUDA kernel + batch executor exist (`csmv_kernel.cu`, `csmv_batch_executor.cu`).
TLA+ model at `docs/proofs/CSMV.tla`.

### 5.3 Third backend: AccelerateSTM (obstruction-free)

**Goal**: Implement AccelerateSTM (Perlin et al., SBAC-PAD 2025) — obstruction-free
with warp-level cooperative groups-based GC. Uses locator table instead of locks.

**Status**: Planned. Depends on 5.1 (kernel framework) and 5.2 (shared infrastructure).

### 5.4 TLA+ model for GPU STM backends

**Goal**: Model PR-STM priority-stealing protocol and 32-bit lock encoding as a
TLA+ spec. Share structure with existing `TL2.tla` / `TinySTM_WBCTL.tla` models.

**Verification**: TLC model checking for safety invariants (lock inv, atomicity)
and liveness (freedom from priority inversion deadlock).

**Status**: Planned.

### 5.5 Benchmark port

**Goal**: Adapt bank, fuzz_counter, and STAMP subset to GPU execution. Each
transaction's body runs inside a CUDA kernel. Data structures allocated in
GPU global memory via `cudaMalloc`.

**Status**: Planned. Depends on 5.1.

## Prioritization (updated)

| Priority | Item | Effort | Impact | Depends on |
|----------|------|--------|--------|------------|
| P0 | 5.1 PR-STM backend (CUDA C++, Option A) | 2 weeks | Medium (new platform) | — |
| P1 | 5.1b PR-STM persistent kernel (Option B) | 2 weeks | Medium | 5.1 |
| P2 | 5.4 TLA+ model for PR-STM | 1 week | Medium | 5.1 |
| P3 | 5.2 CSMV backend | 3 weeks | Medium | 5.1 |
| P4 | 5.5 Benchmark port | 2 weeks | Medium | 5.1 |
| P5 | 5.3 AccelerateSTM | 3 weeks | Medium | 5.1, 5.2 |

## Verification

Each item must be verified before marking done:

- **5.1**: PR-STM passes `test_tx` (single-thread warp) and `test_ds` (linked-list, rbtree) on GPU
- **5.1b**: Persistent-kernel variant achieves throughput within 2× of ideal (launch amortized)
- **5.2**: CSMV passes all tests with read-only transactions never aborting
- **5.3**: AccelerateSTM passes all tests; no lock-acquisition path in device code
- **5.4**: TLC checks PR-STM spec with 2+ threads, finds no safety violations
- **5.5**: Bank benchmark on PR-STM conserves money, fuzz_counter passes invariant
