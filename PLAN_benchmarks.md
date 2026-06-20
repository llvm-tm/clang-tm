# Benchmark Fidelity Fix Plan — Status

Based on the 2026-06-19 Rust benchmark audit, updated 2026-06-20.

## P0: C++ bug (✅ DONE)

- [x] **intruder.cpp:156**: Non-TM read of `g_decoder_flows[idx].data[i]` — changed to `tm_read_i1()`.

## P1: Major fidelity gaps

- [x] **Vacation best-price**: All three versions (C++ plugin, C++ explicit, Rust) use `>` / highest price (profit for tables). Already consistent — no fix needed.
- [x] **Bank --queue mode**: Added `--queue` flag to `fuzz_bank.rs`. Routes through `QueueExecutor` when set.
- [x] **Bayes --test mode**: Enhanced Rust `test()` from 2 trivial checks to 15 assertions covering CLI defaults, RNG determinism, d2l/l2d roundtrip, penalty formula, and density LL with C++-matching synthetic data.

## P2: Significant rewrites

- [x] **Kmeans TM scope**: Centroid bypass via `unsafe { *ptr() }` is faithful to C++ (same direct array access on TM-allocated memory). No fix needed.
- [x] **Yada --test mode**: Enhanced Rust `test()` with RNG determinism test (1000 values). Geometry tests already present.
- [ ] **Genome rewrite**: DEFERRED. Rust version does string generation + dedup + matching (same algorithm as C++), just sequentially. Exercises TM minimally but correctly. Full rewrite (~600 lines) not warranted for P2.
- [ ] **SSCA2 rewrite**: DEFERRED. Rust version does clique-based graph gen + CSR + triangle counting (same as C++). Triangle count partially bypasses TM. Full rewrite (~400 lines) not warranted for P2.

## P3: Minor alignment

- [ ] **--test mode for standalone CLI bins**: `stamp_bayes.rs`, `stamp_yada.rs` standalone binaries lack `--test` flag. The unified `stamp.rs` has `--test -b <bench>` which is the canonical path.
- [ ] **CLI args alignment**: Named vs positional argument consistency across all benchmarks. Minor.

## Verification

- [x] All changes verified with `cargo test -- --test-threads=1` in `simulator/` (26/26 PASS)
- [x] Rust workspace compiles (`cargo check` in `expli_instr/rust/workspace`)
- [x] Bayes `--test`: All 15 tests pass
- [x] Yada `--test`: All 1008 tests pass
- [x] Bank `--queue`: Compiles cleanly
- [x] C++ intruder: Fixed (TM read wrap)
