# Contributing

Thanks for contributing to clang-tm! This repo implements Software
Transactional Memory (STM) across many backends, with two instrumentation
pipelines (explicit C++/Rust API + an LLVM 22 plugin) and a deterministic
Rust simulator.

Read [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md) for the full build
reference and [`docs/README.md`](docs/README.md) for the doc index.

## Toolchain

`llvm-config-22` / `clang++-22` (plugin only), `cargo` / `rustc` (Rust),
`make`, `cmake` (IDE import only). Run `./tools/check-requirements.sh` to
verify. See [`docs/REQUIREMENTS.md`](docs/REQUIREMENTS.md).

## Quick verification

Before you push, the fast smoke gate must pass:

```sh
make check-fast   # ~60s: plugin + 3 C++ backends + Rust sim + workspace
```

Formatting is enforced in CI (`make fmt-check`). Run it locally before opening
a PR:

```sh
make fmt          # apply clang-format (C++) + rustfmt (Rust)
make fmt-check    # verify: clang-format --Werror + cargo fmt --check + clippy -D warnings
```

### Local pre-commit hooks

The same checks run on every commit via [`pre-commit`](https://pre-commit.com)
(see [`.pre-commit-config.yaml`](.pre-commit-config.yaml)): `clang-format-22` on
staged C/C++ (scoped to the same dirs as `make fmt-check`), `cargo fmt --check`
on the Rust workspaces, plus trailing-whitespace, end-of-file, and YAML checks.

```sh
pip install pre-commit          # once
pre-commit install              # wire into git (runs on `git commit`)
pre-commit run --all-files      # or run everything now, without committing
```

The hooks reuse the repo-pinned `clang-format-22` (not a generic `clang-format`),
so the Toolchain requirements above must be met.

## How to add a C++ backend

1. Create `backends/tm_impl/<name>/<Name>_runtime.cpp` and `<name>.hpp`.
2. Implement the 22+ `static` hook functions (begin/end, read/write for 7
   types, alloc, env).
3. Build a `TMRealHooks` registration table with all function pointers.
4. Register via `tm_register_real_hooks()` in `tm_init()`.
5. Add an `ifeq (BACKEND,<NAME>)` block in `benchmarks/cpp/Makefile`.
6. Test: `make -C benchmarks/cpp bin/test_tx BACKEND=<NAME>` and
   `bin/test_ds BACKEND=<NAME>` (114 / 207 tests).

Hooks must be `static`. In `LLVM_TM_PLUGIN` builds, `tm_init`/`tm_exit`/
`tm_init_thread`/`tm_exit_thread` are DATA variables (function pointers), not
TEXT functions — keep the `#ifdef LLVM_TM_PLUGIN` wrappers.

## How to add a Rust backend

1. Create `explicit_api/rust/workspace/runtime/<name>/` with `Cargo.toml` +
   `src/lib.rs`.
2. Export all `tm_read_*` / `tm_write_*` functions +
   `tm_begin`/`tm_commit`/`tm_abort`/lifecycle.
3. Optionally add `pub mod sim` (7 exported fns) + a `TxState`
   (`Clone + Serialize + Deserialize`) for simulation.
4. Register in `tm/Cargo.toml` and `tm/src/lib.rs`.
5. Add the exclusivity check in `tm/src/lib.rs`.
6. Test: `cargo test --features <name> -p tm` (use `--test-threads=1` for the
   workspace `tm` crate to avoid a pre-existing Condvar race).

## Commit style

- Imperative, concise subject (e.g. `NOrec: fix RO→RW promotion`); wrap the
  body at ~72 cols if it needs context.
- One logical change per commit. Reference the `review-02` S-item or
  `CORRECTNESS_FIXES.md` section where relevant.
- Correctness fixes get a section in
  [`docs/CORRECTNESS_FIXES.md`](docs/CORRECTNESS_FIXES.md) (root cause → fix →
  verification).

## Before you push / open a PR

- [ ] `make check-fast` passes.
- [ ] `make fmt-check` passes (or run `make fmt` to fix).
- [ ] Backend-specific tests run (see the backend sections above).
- [ ] No build artifacts or secrets committed (`**/bin/`, `m5out/` are
      gitignored).
- [ ] `CHANGELOG.md` has a `## Session YYYY-MM-DD — <title>` entry describing
      the change (open work goes in `TODO.md`).

## Where things live

- Open work items: [`TODO.md`](TODO.md) (P0/P1/P2).
- Session history: [`CHANGELOG.md`](CHANGELOG.md).
- Per-backend reference: [`docs/IMPLEMENTATIONS.md`](docs/IMPLEMENTATIONS.md).
- TLA+ models: [`docs/proofs.md`](docs/proofs.md).
- Missing-instrumentation diagnosis:
  [`docs/INSTRUMENTATION_DEBUGGING.md`](docs/INSTRUMENTATION_DEBUGGING.md).
