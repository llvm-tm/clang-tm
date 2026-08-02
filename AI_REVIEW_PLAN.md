# TM API C++ — AI Technical Review & Improvement Plan

> Generated on 2026-08-02 by the AIedu pipeline:
> 1. Agentic summarization (opencode DeepSeek V4 Flash) produced a dossier of the
>    docs, PlusCal/TLA+ specs, implementation notes, and implementations.
> 2. IAedu gpt-5.5 wrote this technical review (strong/weak points) and the
>    improvement plan below, grounded strictly in that dossier.
>
> Dossier: `/tmp/tm_dossier.md` at generation time. Review is chat-only; the
> reviewer has no direct file access, so line numbers/test results are
> dossier-reported facts and must be confirmed against the code.

# Technical review

## A. Strong points

### 1. Ambitious and coherent architecture

The project has a clear research architecture: one transactional-memory semantic API exercised across many implementations, formal models, Rust ports, simulators, and benchmarks. That is a strong design for cross-validation because implementation bugs can be found by comparing several independent artifacts.

Key examples:

- Explicit C++ API in `expli_instr/cpp/include/tm_api.hpp`.
- LLVM/Clang instrumentation plugin in `plugin/`, with the documented five-pass Honorio pipeline:
  - `tm-clone`
  - `tm-collect`
  - `tm-redirect`
  - `tm-instrument`
  - `tm-cleanup`
- Runtime-swappable backend hook table through `TMRealHooks`, `tm_swap_runtime()`, and `tm_register_real_hooks()` in `backends/tm_impl/common/tm_hooks.hpp` and `backends/tm_impl/common/tm_hooks.cpp`.
- Rust workspace in `expli_instr/rust/workspace/`, with backend selection via cargo features and `exclusive_backend!`.

The hook-table architecture is especially useful for experimentation: the same frontend can drive TL2, NOrec, TinySTM, SGL, TSX, GPU-oriented runtimes, persistent runtimes, and distributed variants without rewriting the API layer.

### 2. Broad backend coverage

The backend inventory under `backends/tm_impl/` is unusually broad. It includes:

- Version-clock OCC backends such as `tl2`, `norec`, `tsc_tm`, `romulus`, `leftright`, `mvlog`.
- Word-lock / LSA systems such as `tiny_stm` and `swisstm`.
- Single-lock baselines such as `single_global_lock`, `persistent_sgl`, and `distributed_sgl`.
- HTM / persistence-oriented systems such as `tsx_sgl`, `spht`, `nvhtm`, and `dudetm`.
- GPU-oriented systems such as `gpu_stm`, `csmv`, `gputx`, `gacco`, and `gpu_gust`.
- Distributed and deterministic systems such as `tikv` and `calvin`.

That breadth makes the project valuable as a comparative TM laboratory, not just as one backend implementation.

### 3. Shared runtime infrastructure is well factored

The common runtime layer appears to solve several hard cross-cutting problems in one place:

- Hook contract and DATA-symbol rule in `backends/tm_impl/common/tm_hooks.hpp`.
- Shared TLS state, hook application, and backend swapping in `backends/tm_impl/common/tm_hooks.cpp`.
- Macro-based hook generation in `backends/tm_impl/common/tm_backend_macros.hpp`.
- Address classification and plugin bypass checks in `backends/tm_impl/common/tm_common.hpp`.
- Region allocator and `.tm_shared` global registration in `backends/tm_impl/common/tm_region_allocator.hpp` and `backends/tm_impl/common/tm_region_allocator.cpp`.

The dossier says `tm_backend_macros.hpp` removed about 1,000 lines across 14 backends. That kind of consolidation is important here because the project has many backends implementing similar read/write hook surfaces.

### 4. Formal verification culture is unusually strong

The project contains a large formal-model corpus under `docs/proofs/`: 38 `.tla` files and 77 `.cfg` files. That is a serious investment.

Notable strengths:

- `SGL.tla` has the only full mechanical TLAPS proof, with 42/42 obligations proved, according to `docs/proofs/README.md`.
- Many models include concrete safety invariants rather than only high-level sketches.
- `NOrec.tla` models torn reads using a three-step split, which the dossier says fixed an important modeling issue.
- `TMTypes.tla` centralizes shared operators and fence-fidelity machinery.
- GPU models such as `GPU_PR_STM.tla`, `GPU_JVSTM.tla`, and `GPU_GUST.tla` explicitly model SIMT / warp behavior, which is rare and valuable.

The verification layer has already found or clarified real implementation issues, including the NOrec torn-read protocol, PersistentSGL dual-write modeling, LEFTRIGHT fence fixes, and Romulus read-validation behavior.

### 5. Good documentation discipline

The dossier points to several useful documentation anchors:

- `docs/DEVELOPER_GUIDE.md`
- `plugin/README.md`
- `docs/IMPLEMENTATIONS.md`
- `docs/proofs/README.md`
- `docs/proofs/TLA_REVIEW.md`
- `docs/queue_execution_model.md`
- backend-specific notes such as `spht/Implementation_notes.md`, `xtm/Implementation_notes.md`, `mvlog/Implementation_notes.md`, and `tikv/README.md`.

The documentation seems to distinguish algorithm papers, actual C++ behavior, Rust ports, and formal models. That distinction matters because several backends intentionally or accidentally diverge from their papers.

### 6. The project is honest about known bugs

A major strength is that the dossier does not present the project as more mature than it is. It explicitly calls out severe correctness bugs and fidelity gaps, for example:

- NOrec plugin-mode bypass in `backends/tm_impl/norec/NOrec.hpp`.
- PersistentSGL crash-window issue in `backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp`.
- TL2 `abort_tx()` lock-release fragility in `backends/tm_impl/tl2/tl2.hpp`.
- NVHTM model/implementation mismatch between `docs/proofs/NVHTM.tla` and `backends/tm_impl/nvhtm/NVHTM_runtime.cpp`.
- SPHT persistent commit log mismatch between `docs/proofs/SPHT.tla` and `backends/tm_impl/spht/SPHT_runtime.cpp`.

That level of transparency makes the project easier to improve.

### 7. Simulator and calibration work is careful

The Rust simulator under `simulator/` appears to be more than a toy. It includes:

- Backend dispatch in `simulator/src/backend.rs`.
- Discrete-event engine in `simulator/src/engine.rs`.
- Real-backend trace driver in `simulator/src/sim_engine.rs`.
- Cost modeling in `simulator/src/cost_model.rs`.
- Machine and workload profiles in `simulator/src/machine_profile.rs` and `simulator/src/workload_profile.rs`.
- Calibration infrastructure in `simulator/src/calibration.rs`.

The dossier highlights an important calibration lesson: event logging overhead dominated true TM cost by 116×, so calibration must use uninstrumented backends. That shows methodological care.

### 8. Queue runtime is a meaningful performance direction

The queue runtime in `backends/tm_impl/queue/queue_runtime.cpp` and `backends/tm_impl/queue/queue_runtime.h` addresses a real TM performance pain point: decoupling application threads from transactional execution. The dossier reports 27–37% faster performance and 62% fewer aborts on bank, documented in `docs/queue_execution_model.md`.

Even if those numbers need independent reproduction, the design direction is plausible and well integrated with the plugin/runtime architecture.

---

## B. Weak points and risks

### 1. Highest correctness risk: NOrec plugin-mode address bypass

The most serious open implementation bug is in `backends/tm_impl/norec/NOrec.hpp`, specifically dossier-reported lines `416–419` and `493–499`.

The bug: under `LLVM_TM_PLUGIN`, addresses outside the TM region and unregistered globals are bypassed entirely. According to the dossier, that means heap-allocated TM data can receive zero TM tracking in plugin-instrumented binaries.

Observed effects reported in the dossier:

- Lost-update race.
- `social_tm` failure.
- Bank money creation, reported as `1490/64000`.

This is especially dangerous because the TLA+ models assume addresses are TM-tracked; they do not model the plugin address-classification layer. The relevant common infrastructure is in `backends/tm_impl/common/tm_common.hpp`, `backends/tm_impl/common/tm_region_allocator.hpp`, and `backends/tm_impl/common/tm_region_allocator.cpp`.

### 2. Hook DATA/TEXT symbol conflicts are a recurring systemic hazard

The dossier repeatedly emphasizes that every hook in `backends/tm_impl/common/tm_hooks.hpp` must be a function-pointer DATA variable, not a same-named TEXT function.

This is not just theoretical. The dossier says this class of bug has affected:

- `tm_calloc` in STAMP.
- `tm_sigsetjmp` / Linux `__sigsetjmp`.
- `tm_get_env` and `tm_set_jmpbuf`.
- Queue hooks such as `tm_enqueue`, `tm_wait_prev_tx`, `tm_init_thread`, and `tm_exit_thread`.
- `tm_stub_runtime.cpp`.

The risk is severe: the linker can resolve what should be a DATA symbol to function machine code, causing indirect calls to jump to garbage and crash with SIGSEGV or SIGBUS.

This architecture is powerful, but it needs automated enforcement. Otherwise every new backend or hook extension can reintroduce the same failure mode.

### 3. Retry mechanism relies on hard-to-model `siglongjmp` behavior

The retry mechanism uses `sigsetjmp` / `siglongjmp` through shared TLS state such as `tm_jmpbuf`, `tm_nested_call_counter`, and `tm_longjmp_ret` in `backends/tm_impl/common/tm_hooks.cpp`.

The dossier says there is no TLA+ meta-model for `jmp_buf` validity and that AGENTS.md documents this as invisible to all models. That is a serious blind spot because jumping to a dead frame would be undefined behavior, and formal models of transaction state will not catch it.

Some mitigations exist, such as TLS copies and `tm_set_env`, but the absence of a runtime or sanitizer-style validity check remains a risk.

### 4. Model fidelity is uneven

The formal-model corpus is broad, but several models are low fidelity relative to the actual C++ implementation.

Specific examples from the dossier:

- `docs/proofs/NVHTM.tla` models checkpoint/recovery and SGL fallback, while `backends/tm_impl/nvhtm/NVHTM_runtime.cpp` is DRAM-only, lacks crash recovery, and uses pass-through mode on RTM failure.
- `docs/proofs/DistributedSGL.tla` models a client-server lock protocol, while `backends/tm_impl/distributed_sgl/DistributedSGL_runtime.cpp` is described as a single-machine mmap spinlock plus 2PC.
- `docs/proofs/DUDETM.tla` models only the durability pipeline, while `backends/tm_impl/dudetm/DUDETM_runtime.cpp` has many atomics and fences not modeled.
- `docs/proofs/SPHT.tla` clears PCL entries on TSX abort, while `backends/tm_impl/spht/SPHT_runtime.cpp` reportedly does not.
- `docs/proofs/DESEngine.tla` models `simulator/src/engine.rs`, not `simulator/src/sim_engine.rs`, despite the SimEngine naming.
- `docs/proofs/TiKV.tla` does not capture async/gRPC behavior from `expli_instr/rust/workspace/runtime/tikv/src/lib.rs` and the C++ shim `backends/tm_impl/tikv/tikv_backend.cpp`.

This means TLC PASS does not uniformly imply implementation confidence. For high-fidelity models like SGL or TinySTM, it is meaningful. For low-fidelity models, it may mostly verify a design sketch.

### 5. Liveness results are weak

The dossier says liveness mostly fails or is structurally uninformative:

- 11 backends fail liveness, often due to starvation.
- `TSXSim.tla` has a liveness property unsupported by TLC v2.14.
- `JVSTM.tla`, `MVLog.tla`, and `Calvin.tla` have documented false-negatives from unbounded `either`.
- Some GPU properties are defined but not asserted in TLC configs, such as `GPU_PR_STM.tla` and `GPU_SIMT.tla`.

So the safety story is much stronger than the progress story. This is acceptable for a research toolkit if documented, but risky if users interpret liveness coverage as production-grade progress proof.

### 6. Memory-ordering modeling is incomplete

The dossier identifies memory ordering as one of the weakest modeled dimensions.

Known issues:

- `lastFence` cannot distinguish `atomic_signal_fence` from `atomic_thread_fence`.
- Bundled RMW ordering is not represented precisely.
- All `lastFence` backends reportedly have 2–3 misannotated ordering points.
- Rust ports sometimes use stronger `fence(SeqCst)` patterns than the C++ code.
- The LEFTRIGHT ARM plain-load-before-acquire hazard is invisible to the model.

Relevant files include `docs/proofs/TMTypes.tla`, backend models under `docs/proofs/`, and C++ implementations such as `backends/tm_impl/leftright/leftright.hpp`, `backends/tm_impl/tl2/tl2.hpp`, `backends/tm_impl/romulus/romulus.hpp`, and `backends/tm_impl/xtm/xtm.hpp`.

This is a major risk because TM correctness often depends on subtle ordering, especially on weakly ordered architectures.

### 7. PlusCal source and generated TLA+ can desynchronize

The dossier says TSXSGL, TL2, SwissTM, LEFTRIGHT, XTM, and Romulus have `lastFence` annotations only in generated TLA+ translations, not in the PlusCal source. Retranslating the PlusCal would lose those annotations.

This is a tooling/process risk in `docs/proofs/`: the source of truth is ambiguous. A future developer could regenerate specs and silently drop important memory-ordering annotations.

### 8. Persistent-memory claims are risky

Several persistent or NVM-flavored backends have gaps.

Examples:

- `backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp` lacks a fence between `*addr = val` and `memcpy`, according to the dossier. That creates a crash window where persistent data may be stale.
- `backends/tm_impl/nvhtm/NVHTM_runtime.cpp` is DRAM-only and lacks crash recovery despite the NVHTM model in `docs/proofs/NVHTM.tla`.
- `backends/tm_impl/spht/SPHT_runtime.cpp` may leave stale PCL entries after TSX abort, while `docs/proofs/SPHT.tla` assumes they are cleared.
- `backends/tm_impl/dudetm/DUDETM_runtime.cpp` has a low-fidelity model in `docs/proofs/DUDETM.tla`.

The risk is not only correctness but also expectation management: names like NVHTM and PersistentSGL can suggest durability properties stronger than the implementation actually provides.

### 9. Some backend names and documentation are misleading

The dossier calls out several naming/documentation mismatches:

- `backends/tm_impl/romulus/romulus.hpp` is described as version-table OCC, not the Left-Right algorithm.
- `backends/tm_impl/leftright/leftright.hpp` is described as global-clock OCC, not classic Left-Right.
- `docs/proofs/Romulus.tla` and `docs/proofs/LEFTRIGHT.tla` reflect the implemented behavior more than the names suggest.
- `backends/tm_impl/gputx/gputx_runtime.cpp` is not the OSDI'24 "Epic" system.

These mismatches are not necessarily correctness bugs, but they are onboarding and research-reproducibility risks.

### 10. GPU execution coverage is thin

The dossier says GPU kernels and batch executors are largely unverified by local execution because there is no `nvcc` / `hipcc` on the machine. The exception is the PR-STM HIP port, which was verified on AMD gfx1151.

Affected areas include:

- `backends/tm_impl/gpu_stm/`
- `backends/tm_impl/csmv/`
- `backends/tm_impl/gpu_gust/`
- GPU benchmarks under `benchmarks/gpu/`

The formal models are useful, but GPU bugs such as warp divergence, ballot misuse, memory coalescing assumptions, and platform-specific behavior often require hardware execution.

### 11. Rust ports and benchmarks diverge from C++

The project goal includes C++ ↔ TLA+ ↔ Rust cross-validation, but the dossier says Rust fidelity is uneven:

- Rust TL2 adds a global commit lock and six `fence(SeqCst)` operations.
- Rust Romulus has no read-set.
- Rust XTM is version-table OCC, not page-granularity.
- Rust benchmarks have significant simplifications versus C++ in 7 of 10 cases.
- `benchmarks/rust --features mvlog/tl2` fails to build due to `error[E0252]` from `tm-executor` forcing default `wbctl`.

Relevant paths include `expli_instr/rust/workspace/`, `benchmarks/rust/`, and Rust runtime crates under `expli_instr/rust/workspace/runtime/*`.

This weakens the intended three-way cross-validation story.

### 12. Some known implementation bugs remain unresolved

The dossier lists several open or pre-existing issues:

- TL2 `abort_tx()` releases all write-set guards in `backends/tm_impl/tl2/tl2.hpp`, dossier-reported lines `689–692`.
- Calvin `bank -t4` money destruction in `backends/tm_impl/calvin/calvin_runtime.cpp`.
- JVSTM `.peek()` failures because VBox values are not stored at the original address in `backends/tm_impl/jvstm/jvstm_runtime.cpp`.
- GPU_STM_CPU bank money creation under contention in `backends/tm_impl/gpu_stm/`.
- TinySTM / non-TM address assertions in `backends/tm_impl/tiny_stm/`.
- Region allocator double-free false-positive involving deferred frees in `backends/tm_impl/common/tm_region_allocator.cpp`.

The dossier does not say all of these are equally urgent, but they are enough to show that backend maturity varies considerably.

---

# C. Prioritized improvement plan

Effort is qualitative:

- **S**: localized code/config/doc change.
- **M**: nontrivial implementation plus targeted tests.
- **L**: cross-cutting redesign, model rewrite, or broad validation work.

## P0

| Priority | Work item | What to change | Why | Effort | Dependencies |
|---|---|---|---|---|---|
| P0 | Fix NOrec plugin-mode bypass | Replace the blanket plugin bypass in `backends/tm_impl/norec/NOrec.hpp` lines `416–419` and `493–499` with the TinySTM-style `LLVM_TM_ADDR_CHECK` pattern from `backends/tm_impl/common/tm_common.hpp`. Also review commit-path non-TM write-set skipping at dossier-reported lines `276–284`. | This is the highest-priority known correctness bug: heap TM data can be untracked in plugin mode, causing lost updates and bank money creation. | M | Requires understanding `isTMAddress`, `isTMGlobal`, stack-bounds tracking, and `.tm_shared` registration in `tm_region_allocator.hpp/.cpp`. |
| P0 | Add plugin-mode heap-address regression tests | Add a minimal plugin-instrumented test where heap-allocated shared state is accessed transactionally under NOrec. Include bank/social-style regression if already available under `benchmarks/plugin/`. | Prevents the NOrec bug from returning and validates the fragile address taxonomy. | M | Depends on fixing NOrec bypass first. Uses plugin build flow from `plugin/README.md` or `tm_pipeline.mk`. |
| P0 | Enforce hook DATA-symbol contract automatically | Add a build or CI check that scans backend objects/libraries and fails if hook names from `backends/tm_impl/common/tm_hooks.hpp` appear as TEXT symbols instead of DATA variables. | DATA/TEXT hook conflicts are described as the most recurrent crash class in the repo. Automated enforcement is better than relying on review memory. | M | Needs a complete canonical hook list from `tm_hooks.hpp`; must cover queue hooks in `backends/tm_impl/queue/queue_runtime.cpp` and stub/runtime variants. |
| P0 | Harden TL2 abort lock release | Change `backends/tm_impl/tl2/tl2.hpp` so `abort_tx()` only releases locks actually owned by the current transaction, or tracks ownership robustly before clearing guard bits. | The dossier says lines `689–692` can clear a concurrent transaction's lock bit if called in the wrong path. Even if currently masked, this is a fragile correctness dependency. | M | Requires understanding TL2 guard ownership, commit-failure longjmp paths, and write-set structure. |
| P0 | Clearly mark unsafe or non-production backends | Add a visible status table in `docs/IMPLEMENTATIONS.md` for known unsafe modes: NOrec plugin bug until fixed, NVHTM pass-through, Calvin bank contention issue, JVSTM `.peek()` failures, GPU_STM_CPU contention issue. | Prevents users from assuming every backend has equivalent correctness maturity. | S | Depends only on existing dossier facts. |

## P1

| Priority | Work item | What to change | Why | Effort | Dependencies |
|---|---|---|---|---|---|
| P1 | Fix PersistentSGL durability ordering | In `backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp`, add the audit-recommended ordering between `*addr = val` and persistent `memcpy`; evaluate `atomic_thread_fence(seq_cst)` and `msync(MS_SYNC)` as documented by the dossier. Update `docs/proofs/PersistentSGL.tla` if the implementation semantics change. | Current crash window can leave persistent data stale. This directly affects the backend's advertised persistence behavior. | M | Requires deciding the intended persistence contract: mmap durability simulation only, or true crash-consistency discipline. |
| P1 | Decide NVHTM direction | Either rewrite `docs/proofs/NVHTM.tla` to match `backends/tm_impl/nvhtm/NVHTM_runtime.cpp`, or change the C++ runtime to implement the modeled checkpoint/recovery and SGL fallback algorithm. | Current model describes an algorithm that the C++ code does not implement. That makes verification misleading. | L | Requires choosing whether NVHTM is a real durability backend or a DRAM/RTM experimental backend. |
| P1 | Add SGL fallback to NVHTM or disable pass-through semantics | Replace pass-through mode on RTM failure in `backends/tm_impl/nvhtm/NVHTM_runtime.cpp` with a mutual-exclusion fallback similar in spirit to `backends/tm_impl/spht/SPHT_runtime.cpp` or `backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp`. | Pass-through mode provides no TM guarantees on RTM failure, which is a severe semantic hole. | M/L | Depends on NVHTM direction decision. |
| P1 | Fix SPHT PCL abort mismatch | Make `backends/tm_impl/spht/SPHT_runtime.cpp` clear or invalidate PCL entries on TSX abort, or weaken `docs/proofs/SPHT.tla` to match real behavior. | Spec and implementation disagree on recovery-relevant state. Stale PCL entries are a persistent correctness risk. | M | Requires understanding PCL ownership, epoch group commit, and recovery semantics. |
| P1 | Move `lastFence` annotations into PlusCal sources | For TSXSGL, TL2, SwissTM, LEFTRIGHT, XTM, and Romulus, move generated-TLA-only `lastFence` annotations back into PlusCal source files under `docs/proofs/`. | Retranslating PlusCal currently risks silently dropping memory-ordering annotations. | M | Requires identifying the authoritative PlusCal sources and generated translation artifacts. |
| P1 | Improve memory-ordering model vocabulary | Extend `docs/proofs/TMTypes.tla` or related specs to distinguish compiler barriers, CPU fences, acquire/release RMWs, and plain-load-before-acquire hazards where relevant. | Current `lastFence` abstraction is too coarse and hides issues like the LEFTRIGHT ARM hazard. | L | Depends on deciding the target memory model granularity. |
| P1 | Add trace-based C++ ↔ TLA+ cross-validation | Use traces from selected real backends and replay/check them against high-value models, starting with `NOrec.tla`, `TL2.tla`, `TinySTM_WBCTL.tla`, `SGL.tla`, and `MVLog.tla`. | The dossier says audit step 3 is analysis-only and trace-replayed validation has not been done. This would make the formal layer much more credible. | L | Requires stable trace schema, address abstraction, and mapping real events to model actions. Could build on `simulator/src/sim_engine.rs`. |
| P1 | Add runtime checks for `siglongjmp` frame validity | Add debug-mode instrumentation around `tm_set_jmpbuf`, `tm_set_env`, and retry paths in `backends/tm_impl/common/tm_hooks.cpp` to detect stale or missing jump buffers. | TLA+ cannot catch jumping to a dead frame; runtime checks can catch some classes of misuse. | M | Requires care not to perturb release-mode performance or signal-safety assumptions. |

## P2

| Priority | Work item | What to change | Why | Effort | Dependencies |
|---|---|---|---|---|---|
| P2 | Reclassify liveness as documented limitation or repair it | In `docs/proofs/README.md` and backend cfgs, separate meaningful liveness PASS cases from expected starvation FAILs and known false-negatives. Repair `JVSTM.tla`, `MVLog.tla`, and `Calvin.tla` if liveness is intended to be meaningful. | Current liveness status is weak and easy to misread. | M/L | Requires deciding whether liveness is a project goal or a best-effort annotation. |
| P2 | Repair low-fidelity models | Prioritize `docs/proofs/DistributedSGL.tla`, `docs/proofs/DUDETM.tla`, `docs/proofs/DESEngine.tla`, `docs/proofs/TiKV.tla`, and `docs/proofs/TSXSim.tla`. | These models currently provide limited assurance about their corresponding implementations. | L | Depends on owners deciding which backends matter most. |
| P2 | Assert defined GPU properties in TLC configs | Update configs for `docs/proofs/GPU_PR_STM.tla` and `docs/proofs/GPU_SIMT.tla` so defined progress/invariant properties are actually checked where tractable. Continue `CSMV.tla` invariant completion. | Avoids having properties that look verified but are not asserted. | M | Requires TLC state-space tuning. |
| P2 | Add GPU hardware validation path | Add documented build/test targets for `backends/tm_impl/gpu_stm/`, `backends/tm_impl/csmv/`, `backends/tm_impl/gpu_gust/`, and `benchmarks/gpu/` on CUDA and HIP-capable systems. | GPU bugs are often platform-specific; make-n and include checks are insufficient. | L | Requires access to appropriate CUDA/HIP hardware and toolchains. |
| P2 | Align Rust ports with C++ semantics or label them as variants | For `expli_instr/rust/workspace/`, document or fix divergences such as Rust TL2's global commit lock, Rust Romulus lacking a read-set, and Rust XTM being version-table OCC rather than page-granularity. | The project's cross-validation story depends on the Rust port being comparable to C++ and TLA+. | L | Requires choosing between "faithful ports" and "inspired variants." |
| P2 | Fix Rust benchmark feature build failure | Resolve `benchmarks/rust --features mvlog/tl2` failure caused by `tm-executor` forcing default `wbctl`. | Broken feature combinations reduce benchmark coverage. | M | Requires Cargo feature cleanup under `benchmarks/rust/` and `expli_instr/rust/workspace/`. |
| P2 | Normalize backend naming and docs | In `docs/IMPLEMENTATIONS.md` and backend READMEs, explicitly rename or subtitle misleading backends: Romulus as version-table OCC, LeftRight as global-clock OCC, GPUTX as not Epic. | Reduces research confusion and prevents users from citing the wrong algorithm. | S | No code dependency. |

---

## Recommended execution order

1. **Fix correctness bugs that can corrupt results now**
   - `backends/tm_impl/norec/NOrec.hpp`
   - `backends/tm_impl/tl2/tl2.hpp`
   - `backends/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp`

2. **Add guardrails so the same classes of bugs do not recur**
   - Hook DATA/TEXT symbol checker for `backends/tm_impl/common/tm_hooks.hpp`.
   - Plugin heap-address regression tests.
   - Debug checks around `siglongjmp` state in `backends/tm_impl/common/tm_hooks.cpp`.

3. **Repair misleading verification claims**
   - Resolve `docs/proofs/NVHTM.tla` versus `backends/tm_impl/nvhtm/NVHTM_runtime.cpp`.
   - Move `lastFence` annotations back into PlusCal sources.
   - Clearly label low-fidelity models in `docs/proofs/README.md` and `docs/IMPLEMENTATIONS.md`.

4. **Strengthen cross-validation**
   - Start with trace replay for a small high-value set: SGL, NOrec, TL2, TinySTM WBCTL, and MVLog.
   - Use simulator infrastructure under `simulator/src/sim_engine.rs` if its trace mapping is suitable; the dossier does not provide enough detail to know whether it already emits all required model-level events.

5. **Broaden confidence once core semantics are stable**
   - GPU hardware validation.
   - Rust semantic alignment.
   - Liveness cleanup.
   - Low-fidelity model rewrites.

The project's biggest asset is its breadth plus verification culture. Its biggest risk is that breadth can create a false sense of uniform assurance: some backends are well tested and well modeled, while others are known to be low fidelity, partially implemented, or currently incorrect in specific modes. The improvement plan should therefore prioritize semantic correctness and automated guardrails before adding more algorithms.
