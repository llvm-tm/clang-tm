---
name: Backend proposal
about: Propose adding a new STM backend (C++ or Rust)
title: "[backend] "
labels: backend
---

## Backend name

<!-- The `BACKEND=` token (C++ is uppercase, e.g. MYTM; Rust feature is snake_case). -->

## Language / pipeline

- [ ] C++ (explicit API, `backends/tm_impl/`)
- [ ] Rust (`explicit_api/rust/workspace/runtime/`)
- [ ] LLVM-plugin support needed (DATA-vs-TEXT hook wrappers)
- [ ] Simulation support needed (`pub mod sim` + `TxState`)

## Algorithm summary

<!-- Which STM algorithm: work-stealing, epoch, validation, etc.
     Cite the reference / paper. -->

## Validation plan

<!-- How will you prove correctness?
     - C++: test_tx (114) + test_ds (207) on the new backend
     - Rust: cargo test --features <name> -p tm
     - TLA+ model? (see docs/proofs.md)
     - Simulator fidelity check? -->

## Reference implementation

<!-- Link to an existing implementation (TinySTM, SWISSSTM, etc.) you'll base it on. -->
