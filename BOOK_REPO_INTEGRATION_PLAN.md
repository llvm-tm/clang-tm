# Book ↔ Repository Integration Plan

**Date:** 2026-08-26
**Status:** Draft
**Goal:** Close the gap between the textbook (docs/book/) and the working code (the rest of the repo), so that a reader can go from any book concept to running code and back in ≤3 steps.

---

## Audit Summary

| Dimension | Book | Repo | Gap |
|-----------|------|------|-----|
| Code listings | 31 labeled + ~40 unlabeled (all inline in .tex) | — | **0 listings are `lstinputlisting` from the repo.** Every example is textbook-style; none compile against the actual code. |
| `\rep{}` path refs | 9 | — | Covers only norec, tsx_sgl, power8_htm, xtm, leftright, tikv, gpu/backends — misses >90% of the repo tree. |
| Backend coverage | 25 named in prose | 28 C++ dirs, 18 Rust crates | Book never walks through `tm_hooks.cpp`, `tm_api.hpp`, `tm_region_allocator.hpp`, or any `*_runtime.cpp`. |
| Benchmark coverage | bank, fuzz_counter, fuzz_bank, intruder, STAMP | 16 C++ bins, 12 Rust bins, 12 plugin dirs | eigenbench, rbtree, tpcc, ycsb, stmbench7, DeathStarBench never shown as code. |
| Build/run | app06 has commands | Makefile, Cargo workspace | Never says "here is the code that builds when you run `make`" or "here is the binary and what it does." |
| Debugging/profiling | ch12 mentions rr, fuzzing | stm_bug_tool, patches/debug, patches/profile, simulator | None of these are described as runnable tools. |
| Queue/async | ch07 mentions queue briefly | queue_runtime.cpp, tm-executor crate | No listing, no walkthrough, no example. |
| TLA+ ↔ code | ch06 shows workflow | 24 .tla files + 80 .cfg files | Never shows how a specific .tla maps to a specific C++ backend file. |
| Explicit API | ch04 ch08 show TM<T> usage | tx_executor.hpp, tm_api.hpp, containers/ | Never shows the actual header or explains what `TM<int>` expands to. |
| Stale refs | — | — | `expli_instr/` → `explicit_api/` (renamed); book has stale paths in app06. |

---

## Plan: 7 Work Items (ordered by impact)

### 1. Repository Walkthrough Appendix (NEW: app09)

**Problem.** A reader finishes the book and has no map of the actual repository. They know the theory but not where to find the code that implements it.

**Deliverable.** New appendix `app09.tex`: "The Companion Repository — A Guided Walk"

**Contents:**
- `§1 — Tree overview`. Full annotated tree diagram with 1-line descriptions for every top-level directory and the 5 most important subdirectories.
- `§2 — The hook system`. Walk through `backends/tm_impl/common/tm_hooks.cpp` (50 lines): the function-pointer table, `tm_register_real_hooks()`, and the `apply_hooks_unlocked()` dispatch. This is the single file that connects all backends to all benchmarks — readers need to see it once.
- `§3 — The explicit API`. Walk through `explicit_api/cpp/expli_tm_api/tm_api.hpp` (80 lines): `TM<T>` wrapper, `transaction()` retry loop, `tm_set_jmpbuf()`. Show what `TM<int>::value_` expands to after the pass.
- `§4 — The hook table per backend`. Pick ONE backend (NOrec) and show its `*_runtime.cpp`: `TMRealHooks` struct, `tm_register_real_hooks()`, and the `#ifdef LLVM_TM_PLUGIN` guard. Show the 5-line mapping from generic hook → backend function.
- `§5 — The benchmark entry points`. Show `benchmarks/cpp/bank/bank.cpp` (60 lines): `main()` → `tm_init()` → thread spawn → `do_transaction_work()` → `tm_end()` — the same program the reader has been reasoning about since Ch.1.
- `§6 — The TLA+ model behind the proof`. Show `docs/proofs/NOrec.tla` (first 40 lines of PlusCal) and the corresponding `docs/proofs/NOrec.cfg`. Map each PlusCal label to the C++ file and function it specifies.
- `§7 — Navigating by keyword`. Index of terms → files: "read-set" → `NOrec.hpp:read_set`, "commit lock" → `tl2.hpp:g_lock`, "deferred free" → `tm_hooks.cpp:tm_flush_deferred_frees`, etc.

**Effort:** ~400 lines of LaTeX. One file per section, or one monolithic appendix.

---

### 2. Replace Textbook Listings with Real Repo Listings (ch01–ch19)

**Problem.** The book's 31 labeled listings are all written directly in the `.tex` files. They look like the code but aren't the code. A reader who copies a listing into an editor gets something that doesn't compile against the repo.

**Approach.** Replace the most important listings with `\lstinputlisting` from the actual repo, then add prose around them explaining what the reader is seeing. Keep the inline listings for pedagogical simplifications (TLA+ models, pseudocode) — only swap the C++/Rust listings that claim to be compilable.

**Priority replacements (15 listings):**

| Listing | Current | Replacement source |
|---------|---------|--------------------|
| ch01: `lst:transfer-basic` | Inline C (20 lines) | `benchmarks/cpp/bank/bank.cpp:do_transaction_work()` (extracted, ~15 lines) |
| ch04: `lst:rust-bank` | Inline Rust (30 lines) | `explicit_api/rust/workspace/tm/examples/simple.rs` |
| ch04: `lst:abi-safe-bank` | Inline C++ (30 lines) | `explicit_api/cpp/expli_tm_api/tm_api.hpp` excerpt (TM<T> wrapper) |
| ch08: `lst:calvin-state/exec/demo` | Inline Rust (80 lines total) | `backends/tm_impl/calvin/calvin_runtime.cpp` excerpts |
| ch09: `lst:norec-rust-*` (5 listings) | Inline Rust (200 lines) | `docs/book/listings/norec_rust_complete.rs` (already on disk) — or `explicit_api/rust/workspace/runtime/norec/src/lib.rs` |
| ch10: unlabeled LLVM IR | Inline C (50 lines) | Generate from `plugin/` — add a `make -C plugin sample-ir` target |
| ch12: abort-fuzzing harness | Inline C (40 lines) | New file: `benchmarks/cpp/bank/bank_fuzz_abort.cpp` (build target) |
| ch14: SGL bank benchmark | Inline C++ (40 lines) | `benchmarks/cpp/bank/bank.cpp` (SGL path) |
| ch16: RTM transfer | Inline C (30 lines) | New file: `benchmarks/cpp/tsx_sgl_demo/tsx_transfer.cpp` |

**Effort:** ~8 new source files in benchmarks/cpp or docs/book/listings, + `\lstinputlisting` edits across 10 chapters. ~200 lines of LaTeX change.

---

### 3. New Section: "The Hook System" (ch07 or ch08)

**Problem.** The hook system (`tm_hooks.cpp`, `tm_hooks.hpp`) is the central dispatch that connects all backends to all benchmarks. The book mentions `tm_begin`/`tm_end` as API calls but never shows the function-pointer table or explains how a backend registers.

**Deliverable.** New section `sec:hook-system` in ch07 ("Transaction API Design") or ch08 ("Non-Speculative Designs").

**Contents:**
- 20-line listing: `tm_hooks.hpp` struct `TMRealHooks` — the 20 function pointers.
- 10-line listing: `tm_hooks.cpp:s_hooks` global — the registered table.
- 10-line listing: `tm_hooks.cpp:tm_read_i4()` — the generic dispatch: `s_hooks.read_i4(addr)`.
- Prose: how `tm_register_real_hooks()` replaces stubs with real functions; when it happens (first `tm_init()` call).
- Cross-reference to the `#ifdef LLVM_TM_PLUGIN` guard in `*_runtime.cpp` files.

**Effort:** ~100 lines of LaTeX + 3 small listings.

---

### 4. New Section: "Queue Execution and Async TM" (ch07 or ch18)

**Problem.** The queue runtime (`queue_runtime.cpp`, `tm-executor/src/lib.rs`) enables async TM execution — a worker thread pulls enqueued TX functions and runs them transactionally. This is a major feature of the repo but the book mentions it only in passing (ch07 mentions `tm_enqueue`; ch18 mentions persistent queues).

**Deliverable.** New section `sec:queue-execution` in ch07 or ch18.

**Contents:**
- Diagram: enqueue → queue → worker loop → begin/body/end/retry.
- 15-line listing: `queue_runtime.cpp:real_tm_enqueue()` — the enqueue path.
- 20-line listing: `tm-executor/src/lib.rs:QueueExecutor::worker_loop()` — the worker path.
- Prose: how `test_queue_multi.cpp` demonstrates 4 threads enqueuing concurrent transfers.
- Exercise: "Modify the queue executor to add a timeout; what happens to in-flight transactions?"

**Effort:** ~120 lines of LaTeX + 1 diagram + 2 listings.

---

### 5. New Section: "Debugging and Profiling Tools" (ch12)

**Problem.** Ch12 mentions `rr`, fuzzing, and deterministic scheduling. The repo has three actual debugging/profiling systems that are never described:
1. `tools/stm_bug_tool/` — event parser, invariant checker, timeline visualizer.
2. `patches/debug/` — 6 reversible debug-print patches with `apply.sh`/`remove.sh`/`status.sh`.
3. `patches/profile/tsx/` — RDTSC TSX profiling workflow with `run_workflow.sh`.

**Deliverable.** New section `sec:debugging-tools` in ch12.

**Contents:**
- `§A — The stm_bug_tool`. 10-line example: pipe a trace through `event_parser.py`, `invariant_checker.py`, `timeline_viz.py`.
- `§B — Reversible debug patches`. 5-line listing: `patches/debug/apply.sh 003-tinystm-wbctl-debug.patch` → adds ASSERT debug dump → `remove.sh` reverts. Explain why this workflow (patch-based, not `#ifdef`) keeps source clean.
- `§C — TSX profiling workflow`. Walk through `patches/profile/tsx/run_workflow.sh`: applies RDTSC patch → builds → runs fuzz_counter + bank → produces CSV + calibration JSON → reverts. Show 3 lines of output.
- Cross-reference to ch14 (`sec:perf-calibration`) for how the profiling output feeds the simulator cost model.

**Effort:** ~150 lines of LaTeX + 3 small listings.

---

### 6. New Section: "Your First TLA+ Proof → Your First Code Fix" (ch06)

**Problem.** The book shows TLA+ and C++ as separate worlds. The most powerful workflow in the repo is: TLC finds a bug → you fix the code → TLC confirms the fix. This loop is never demonstrated.

**Deliverable.** New worked example `sec:tla-to-code-fix` in ch06.

**Contents:**
- Step 1: Write a 15-line PlusCal model of a broken transfer (omit rollback on abort).
- Step 2: Run TLC → counterexample: money lost.
- Step 3: Find the corresponding C++ bug — `NOrec.hpp` commit path skips write-back for non-TM addresses (the known plugin-mode bug documented in IMPLEMENTATIONS.md).
- Step 4: Fix the code (1 line: replace `#ifdef LLVM_TM_PLUGIN` bypass with `LLVM_TM_ADDR_CHECK`).
- Step 5: Re-run TLC → clean. Re-run `make -C benchmarks/cpp BACKEND=NOREC run-bank` → money conserved.
- Exercise: "Apply the same fix to TL2 and verify with TLC."

**Effort:** ~200 lines of LaTeX + 1 new .tla file in docs/proofs/.

---

### 7. Stale Path Cleanup + Cross-Reference Audit

**Problem.** After the `expli_instr/` → `explicit_api/` rename, some book paths are stale. Additionally, many `\rep{}` and `\texttt{}` path references don't match actual repo paths.

**Specific fixes needed:**
1. **app06:117** — `pr_stm_cpu.cpp` path → should be `backends/tm_impl/gpu_stm/cpu/pr_stm_cpu.cpp` (or verify the short path works).
2. **ch03** — `backends/tm_impl/power8_htm` → verify this directory exists (it does: `POWER8HTM_runtime.cpp`).
3. **ch08:63** — `explicit_api/cpp/include/tx_executor.hpp` → check if this is the canonical path or if `expli_tm_api/tx_executor.hpp` is.
4. **app06:120+** — scan for any remaining `expli_instr` references.
5. **All `\rep{}` paths** — verify each resolves to a real file. Currently 9 references; verify all 9.
6. **Make commands** — verify every `make -C ...` target in the book actually exists in the Makefile. In particular: `run-stamp`, `run-bank`, `run-fuzz-counter`, `run-test-tx`, `run-test-ds`, `run-intruder`.

**Effort:** 1–2 hours of grep + fixups. ~30 lines of LaTeX change.

---

## Execution Order

| Phase | Items | Est. lines | Dependencies |
|-------|-------|------------|-------------|
| **P0** (cleanup) | #7 stale paths | ~30 | None |
| **P1** (high-value) | #1 repo walkthrough appendix, #3 hook system section | ~500 | None |
| **P2** (integration) | #2 real listings, #6 TLA→code fix | ~400 | #1 (walkthrough provides context) |
| **P3** (new coverage) | #4 queue execution, #5 debugging tools | ~270 | None |
| **Total** | | **~1200 lines** | |

---

## What NOT to Change

- **Textbook-style TLA+ listings** — keep inline. TLA+ models are self-contained by design; `lstinputlisting` would force readers to navigate docs/proofs/.
- **Rust pseudocode in ch08/09** — keep inline. The Rust listings in ch08 (Calvin) and ch09 (NOrec) are pedagogically simplified; the real code has more edge cases that would distract.
- **GPU kernel listings** — keep inline. The CUDA/HIP kernels are platform-specific and wouldn't compile for most readers.
- **Inline C++ listings that are self-contained** (e.g. ch03 TSO store-buffer, ch11 DSE/LICM demos) — these are pedagogical, not benchmark code.

---

## Open Questions for the User

1. **app09 as a new appendix vs. expanding app06?** The dev-setup appendix already covers building. A new appendix would add ~400 lines but keep app06 focused. Alternatively, merge into app06 under a new "Navigating the Code" section.
2. **How many real listings to swap?** The plan suggests 15 priority swaps. Each requires creating a compilable source file in benchmarks/cpp or docs/book/listings/ and adjusting the surrounding prose. Could be done incrementally.
3. **TLA→code fix example (#6):** Use the known NOrec plugin-mode bug (IMPLEMENTATIONS.md documents it), or a simpler model bug (e.g. the no-rollback counterexample)?
