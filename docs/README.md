# Documentation Index

Where to start, and what each document is for. For the "how do I build and
contribute" walkthrough, go straight to [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).
To contribute, read [`../CONTRIBUTING.md`](../CONTRIBUTING.md) (add-a-backend
walkthrough, test suite, commit style, PR checklist). For the full list of open
work items, see [`../TODO.md`](../TODO.md).

## Getting started

| Document | What it is | Read it when… |
|----------|-----------|---------------|
| [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md) | Build, test, and add-a-backend walkthrough | You are new to the repo and want to build something or add a backend |
| [REQUIREMENTS.md](REQUIREMENTS.md) | Software requirements + per-platform install + `check-requirements.sh` | Your build is failing on missing LLVM/Clang/toolchain |
| [IMPLEMENTATIONS.md](IMPLEMENTATIONS.md) | Reference for every STM/HTM/distributed backend | You want to understand or compare a specific backend's algorithm |
| [BACKEND_COMPARISON.md](BACKEND_COMPARISON.md) | WBCTL vs NOrec vs SGL across STAMP/TPC-C/STMbench7 | You are choosing a backend for a workload |

## Correctness & debugging

| Document | What it is | Read it when… |
|----------|-----------|---------------|
| [CORRECTNESS_FIXES.md](CORRECTNESS_FIXES.md) | Status of each known-correctness fix (root cause → fix → verification) | A correctness bug is reported and you want the history/fix |
| [BUG_ASSESSMENT.md](BUG_ASSESSMENT.md) | `_tm_clone` + LLVM `-O3` broken-IR assessment (STL in TM) | STL containers inside a TX misbehave |
| [EBR_DOUBLE_FREE_ANALYSIS.md](EBR_DOUBLE_FREE_ANALYSIS.md) | TinySTM EBR double-free (fixed) | Investigating heap corruption in TinySTM |
| [INSTRUMENTATION_DEBUGGING.md](INSTRUMENTATION_DEBUGGING.md) | How to diagnose *missing* TM instrumentation | A TM program races/leaves money unconserved and you suspect an un-instrumented access |
| [plugin-debug.md](plugin-debug.md) | Debug builds, GDB, and the plugin test binaries | You need to step through the plugin or a test |
| [proofs.md](proofs.md) | TLA+ / formal correctness models per backend | You want the formal model or to run TLC |
| [POST_MERGE_TEST_PLAN.md](POST_MERGE_TEST_PLAN.md) | Post-merge (GitLab→GitHub) verification plan | Auditing the 2026-09-01 merge |

## Benchmarks & performance

| Document | What it is | Read it when… |
|----------|-----------|---------------|
| [STAMP.md](STAMP.md) | The three STAMP implementations and how to run them | You want to run STAMP (bayes, genome, intruder, …) |
| [benchmark_assessment.md](benchmark_assessment.md) | TSX+SGL performance assessment (vs SGL) | You are evaluating TSX/SGL throughput |
| [comparison-stamp-small.md](comparison-stamp-small.md) | Simulation vs real hardware (STAMP small configs) | Comparing `tm-sim` against native runs |
| [comparison-tsx-vs-gem5.md](comparison-tsx-vs-gem5.md) | Real x86+TSX vs gem5 (small configs) | Validating the gem5 TSX model |

## Design & plans

| Document | What it is | Read it when… |
|----------|-----------|---------------|
| [queue_execution_model.md](queue_execution_model.md) | Queue-based (decoupled) TM execution model | You are working on the async/queue executor |
| [fuzz-tool-plan.md](fuzz-tool-plan.md) | Generic auto-instrumenting TM fuzz tool | You are building/using the fuzz tool |
| [live_app_simulation.md](live_app_simulation.md) | Re-running apps under different interleavings | You want schedule exploration beyond trace replay |
| [address_space_allocator_plan.md](address_space_allocator_plan.md) | TM address-space allocator + stack-pointer validation | Working on region allocation / stale-stack-pointer bugs |
| [gpu_static_txn_batching.md](gpu_static_txn_batching.md) | Static-transaction extraction for GPU batching (Calvin/GAccO/Epic) | Working on GPU deterministic backends |
| [llvm_ir_analysis.md](llvm_ir_analysis.md) | How the plugin instruments the bank benchmark | Learning the instrumentation pipeline |
| [llvm_ir_avltree_recursive.md](llvm_ir_avltree_recursive.md) | IR analysis of the recursive AVL tree | Debugging recursive-data-structure instrumentation |
| [llvm_ir_Stmbench7_assessment.md](llvm_ir_Stmbench7_assessment.md) | Why STMbench7 hangs under TinySTM/TL2 (IR growth) | Investigating STMbench7 hangs |

## Meta

| Document | What it is | Read it when… |
|----------|-----------|---------------|
| [AGENTS.md](AGENTS.md) | Live agent contract: build/test commands, hook invariants, LLVM-version policy, verification gates | You (or an agent) are about to change code in this repo |
| [../CHANGELOG.md](../CHANGELOG.md) | Session log (`## Session YYYY-MM-DD` entries); the historical project summary is the undated section at the top | You want the history of a fix, or where to append your own session notes |
| [sessions/](sessions/) | Long-form per-day session write-ups (referenced from `CHANGELOG.md`) | A `CHANGELOG.md` entry says "see docs/sessions/…" |
| [audits/](audits/) | Per-backend audit notes (one file per backend) | You want a one-page risk/coverage summary for a specific backend |
| [book/](book/) | The companion textbook *"Transactional Memory — From Principles to Practice"* (19 chapters + 9 appendices, LaTeX) | You want the theory behind a backend, or to cite/extend the book |
