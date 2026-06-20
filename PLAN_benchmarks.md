# Benchmark Fidelity Fix Plan

Based on the 2026-06-19 Rust benchmark audit. Some items (Bayes all-threads, Yada all-threads, Kmeans convergence loop) were already fixed since the audit.

## P0: C++ bug (must fix before Rust parity matters)

- [ ] **intruder.cpp:156**: Non-TM read of `g_decoder_flows[idx].data[i]` in the decode loop. Should use `tm_read_i1()` to read data written by the same transaction. Currently uses plain C++ load, which returns stale values with value-logging backends (NOrec, TL2).

## P1: Major fidelity gaps

- [ ] **Vacation best-price selection**: Rust picks highest price (profit for tables) when C++ picks lowest price (savings for customer). Invert the comparison in `vacation.rs`.

- [ ] **Bank --queue mode**: Add `--queue` flag that routes transactions through a `QueueExecutor` instead of executing inline.

- [ ] **Bayes --test mode**: Add CLI `--test` mode that runs the test battery with deterministic RNG and asserts.

## P2: Significant rewrites

- [ ] **Kmeans TM scope**: Centroid updates use `unsafe { *ptr() }` (bypassing TM). Change to TM-tracked writes inside `transaction()` closures.

- [ ] **Yada --test mode**: Add `--test` mode with deterministic verification.

- [ ] **Genome rewrite**: Currently a sequential hash-table insertion benchmark (completely different from C++). Rewrite to match C++: multi-threaded string dedup + TM-mediated segment matching.

- [ ] **SSCA2 rewrite**: Currently uses different graph generation (clique-based vs R-MAT) and minimal TM. Rewrite to match C++: proper synthetic graph generation + CSR + TM-tracked triangle counting.

## P3: Minor alignment

- [ ] **--test mode for all STAMP benchmarks**: Unified `--test` flag runs deterministic test battery.
- [ ] **CLI args alignment**: Named vs positional argument consistency across all benchmarks.

## Verification

- All changes verified with `cargo test -- --test-threads=1` in `simulator/`
- Rust workspace compiles (`cargo check` in `expli_instr/rust/workspace`)
- Plugin tests pass (`make check` in `plugin/`)
- Each benchmark tested with default parameters for correctness (money conservation, counter sum, etc.)
