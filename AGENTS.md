# AGENTS.md — Agent Instructions

Short, stable instructions for AI agents (opencode) working in this repo.
**This file must stay ≤150 lines.** Detailed history lives in
[`CHANGELOG.md`](CHANGELOG.md); open work lives in [`TODO.md`](TODO.md).

## What this repo is

Software Transactional Memory (STM) across 16 C++ backends + a Rust
deterministic simulator, with two instrumentation pipelines (explicit C++/Rust
API and an LLVM 22 plugin) and benchmark suites (STAMP, TPC-C, STMbench7, bank).

## First things to do

```sh
make check-fast        # ~60s smoke: plugin + 3 C++ backends + Rust; run this to verify the toolchain
make help              # list all targets (default target)
```

- Build/test a single backend: `make -C benchmarks/cpp bin/test_tx BACKEND=NOREC`
- Plugin tests: `make -C plugin run`
- Simulator: `cd simulator && cargo test`
- Rust workspace: `cd explicit_api/rust/workspace && cargo test --features wbctl -p tm`

## Where to log work (important)

- **Do NOT append session notes to this file.** Append a `## Session YYYY-MM-DD —
  <title>` entry to [`CHANGELOG.md`](CHANGELOG.md) (or write a longer
  `docs/sessions/YYYY-MM-DD.md`).
- Track open work items in [`TODO.md`](TODO.md) (P0/P1/P2). Inline `TODO.md:`
  comments in source must point at an entry in that file.
- Correctness fixes get a section in [`docs/CORRECTNESS_FIXES.md`](docs/CORRECTNESS_FIXES.md)
  (root cause → fix → verification).

## Documentation map

Start at [`docs/README.md`](docs/README.md) (index of all docs). Key entries:

- [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md) — build, test, add a backend.
- [`docs/REQUIREMENTS.md`](docs/REQUIREMENTS.md) — toolchain + install.
- [`docs/IMPLEMENTATIONS.md`](docs/IMPLEMENTATIONS.md) — per-backend reference.
- [`docs/proofs.md`](docs/proofs.md) — TLA+ models; run via `docs/proofs/Makefile`.
- [`docs/CORRECTNESS_FIXES.md`](docs/CORRECTNESS_FIXES.md) — bug-fix history.
- [`docs/INSTRUMENTATION_DEBUGGING.md`](docs/INSTRUMENTATION_DEBUGGING.md) — missing-instrumentation diagnosis.

## Key conventions

- **Hooks**: every C++ backend registers a `TMRealHooks` table (22 function
  pointers) via `tm_register_real_hooks()`. Hook functions must be `static`.
  In `LLVM_TM_PLUGIN` builds, `tm_init`/`tm_exit`/`tm_init_thread`/
  `tm_exit_thread` are DATA variables (function pointers), not TEXT functions —
  keep the `#ifdef LLVM_TM_PLUGIN` wrappers.
- **Allocation**: `tm_region_malloc` allocates from a fixed mmap'd region;
  inside a TX, `tm_track_spec_alloc` records speculative allocs (cleared on abort).
- **TLS**: `tm_jmpbuf`, `tm_nested_call_counter`, `tm_longjmp_ret` are shared in
  `tm_hooks.cpp`.
- **Simulation**: Rust backends gate TLS behind `#[cfg(feature = "simulation")]`;
  add `pub mod sim` (7 exported fns) + `TxState` (Clone+Serialize+Deserialize)
  for a new sim backend. See `runtime/norec/src/lib.rs`.
- **Default LLVM version is 22** (`llvm-config-22`, `opt-22`, `clang++-22`);
  tooling honors optional `LLVM_VERSION` (CI also probes LLVM 23).
- **Backends**: `TINYSTM WBETL WT NOREC NORECBF SWISSTM TL2 TSC_TM MVLOG SGL
  LEFTRIGHT ROMULUS XTM SPHT TSXSGL` + GPU (`GPU_STM_CPU`, `CSMV`, …).

## Requirements

`llvm-config-22`/`clang++-22` (plugin only), `cargo`/`rustc` (Rust), `make`,
`cmake` (IDE import only — see `CMakeLists.txt` note). Run
`tools/check-requirements.sh` to verify. See
[`docs/REQUIREMENTS.md`](docs/REQUIREMENTS.md).

## Verification before you stop

- C++ backend change → `make -C benchmarks/cpp bin/test_tx BACKEND=<B>` and
  `bin/test_ds BACKEND=<B>` (114 / 207 tests).
- Plugin change → `make -C plugin run`.
- Rust change → `cargo test` in the affected crate (use `--test-threads=1` for
  the workspace `tm` crate to avoid a pre-existing Condvar race).
- Simulator change → `cd simulator && cargo test`.

## Do not

- Append session history to this file.
- Change the default LLVM version away from 22.
- Make hook functions non-`static`.
- Commit secrets/keys or build artifacts (`**/bin/`, `m5out/` are gitignored).
