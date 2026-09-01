# Post-Merge Verification Plan

> **Context**: On 2026-09-01 the `main` branch merged GitLab's 80-commit history
> into the GitHub `main` (merge `6206218`, "GitHub wins for diverged content").
> The merge touched C++ backends, Rust runtimes, the simulator, and docs. This
> plan verifies nothing is broken after that merge. The GitLab remote was removed;
> **`origin` (GitHub) is now the only remote.**

**Status legend**: `[ ]` pending · `[x]` done · `[SKIP]` intentionally skipped

---

## 0. Repo / toolchain sanity

- [ ] `git status` clean on `main` (only intended WIP files uncommitted)
- [ ] `git remote -v` shows only `origin`
- [ ] `git rev-parse HEAD origin/main` equal (`6206218`)
- [ ] Rust toolchain resolves: `rustc --version`, `cargo --version`
- [ ] C++ toolchain: `clang++-22 --version` (or fallback g++)
- [ ] LLVM: `llvm-config-22 --version` (plugin pipeline)

---

## 1. C++ explicit-API correctness (backends)

Primary gate: `benchmarks/cpp` `test_tx` (114/114) + `test_ds` (207/207)
across all backends. The top-level `check-all` target automates this.

```sh
make check-all            # 17 backends: TINYSTM WBETL WT NOREC NORECBF SWISSTM
                          # TL2 TSC_TM MVLOG SGL LEFTRIGHT ROMULUS XTM SPHT
                          # TSXSGL GPU_STM_CPU CSMV
```

- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=TINYSTM`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=WBETL`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=WT`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=NOREC`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=NORECBF`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=SWISSTM`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=TL2`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=TSC_TM`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=MVLOG`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=SGL`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=LEFTRIGHT`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=ROMULUS`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=XTM`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=SPHT`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=TSXSGL`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=GPU_STM_CPU`
- [ ] `make -C benchmarks/cpp bin/test_tx bin/test_ds BACKEND=CSMV`

> Note: `SGL`, `LEFTRIGHT`, `ROMULUS` use explicit `tm_init`/`tm_exit` and are
> skipped by `check-all` — run `./bin/test_tx` / `./bin/test_ds` manually.

### Backend bug-fix regressions (known-tricky backends)
- [ ] `bank -d 500 -a 128 -t 2` + `-t 4`: money conserved (ROMULUS, LEFTRIGHT)
- [ ] `fuzz_counter -t4 -n1000 -c8`: counter sum invariant (CALVIN)
- [ ] `bank -t 4 -d 500 -a 128`: money conserved (CALVIN single-thread OK,
      multi-thread is a known pre-existing issue)

---

## 2. C++ benchmark smoke (expli)

- [ ] `make -C benchmarks/cpp all BACKEND=TINYSTM`
- [ ] `make -C benchmarks/cpp all BACKEND=NOREC`
- [ ] `bank`, `fuzz_counter`, `fuzz_bank` run and report invariants PASS

---

## 3. LLVM plugin pipeline

- [ ] `make -C plugin` builds `libTMInstrument.so`, `libTMRaceChecker.so`,
      `libTMFuzzStrategy.so`
- [ ] `make -C plugin run` — all 18 plugin tests pass
- [ ] `make -C plugin run_noinst` (uninstrumented variants)
- [ ] `make -C plugin test_persist_sgl` (persistence)
- [ ] `make -C plugin/bank bank_tinystm` builds a plugin-instrumented binary
- [ ] `make -C plugin/datastructures bin/avltree_SingleGlobalLock` builds

### Plugin benchmarks (build + smoke)
- [ ] `make -C benchmarks/plugin/bank bank_tinystm` → runs, invariant PASS
- [ ] `make -C benchmarks/plugin/datastructures` → avltree builds/runs
- [ ] `make -C benchmarks/plugin/STAMP stamp_uninstrumented` → kmeans runs
- [ ] DeathStarBench: `make -C benchmarks/plugin/deathstarbench` +
      `social_tm_tinystm_wbctl -d 10000 -u 256 -t 4` invariant PASS

---

## 4. Rust workspace

- [ ] `cargo build --manifest-path explicit_api/rust/workspace/Cargo.toml`
- [ ] `cargo test --manifest-path explicit_api/rust/workspace/Cargo.toml -- --test-threads=1`
      (all backends; `--test-threads=1` avoids the pre-existing QueueExecutor
      hang / mmap-address conflicts)
- [ ] Backend feature sweep builds: `--features wbctl/wbetl/wt/norec/tl2/
      swisstm/dudetm/tsxsgl/nvhtm/spht/leftright/romulus/xtm/tikv/mvlog`
- [ ] `cargo build --manifest-path benchmarks/rust/Cargo.toml`
      (NB: `--features mvlog`/`tl2` fail to build with pre-existing E0252
      because `tm-executor` forces default `wbctl` — not a merge regression)
- [ ] Examples: `simple`, `mt_test`, `time_cells` (mvlog)

---

## 5. Simulator (touched by merge: `verifier.rs`, Cargo.toml)

- [ ] `cargo build --manifest-path simulator/Cargo.toml`
- [ ] `cargo test --manifest-path simulator/Cargo.toml -- --test-threads=1`
      (26 integration tests; `sim_engine_test.rs` + `tsx_ground_truth.rs`)
- [ ] `cargo test -p tm-sim` (SimEngine driver)
- [ ] verifier regression: replay a real trace through `tm-sim --backend norec`,
      confirm no spurious DOUBLE-TX-BEGIN / ABORT-WITHOUT-BEGIN violations
- [ ] `tm-sim --clock-mode cost --machine-profile ...` smoke (cost mode)

---

## 6. gem5 POWER8 HTM (WIP — not part of merge, but adjacent)

These are pre-existing uncommitted WIP files; the merge must not break them.
- [ ] Confirm `gem5_sim/docs/power8-htm-patches.md` + `docs/proofs/Power8HTM.tla`
      still present after merge
- [ ] (If built) `gem5.opt` power target smoke test per plan §9
      — **requires building gem5**; skip if not built

---

## 7. TLA+ proof specs (merged from both branches)

- [ ] `make -C docs/proofs check-one BACKEND=<b>` for each spec
      (TLC jar at `/tmp/tla2tools.jar`)
- [ ] Confirmed GitHub (HEAD) versions retained where both branches had a spec;
      GitLab-only additions (`SimEngine`) present.
- [ ] `make -C docs/proofs verify-liveness` (liveness configs)

---

## 8. Sanity on merge-integrity itself

- [ ] No `expli_instr/` paths anywhere: `git grep expli_instr`
- [ ] `explicit_api/` is the canonical Rust/C++ path
- [ ] `simulator/Cargo.toml` deps all point at `explicit_api/...`
- [ ] `benchmarks/cpp/Makefile` uses `explicit_api/cpp`
- [ ] `.gitignore`, CI workflows (`ci.yml`, `nightly.yml`) consistent with
      `explicit_api/` and include the TLA+ job
- [ ] No `docs/AGENTS.md~organizational_main` temp backup tracked
- [ ] `AGENTS.md` symlink from `docs/` intact

---

## Execution order (recommended)

1. §0 toolchain sanity → 2. §3 plugin (`make -C plugin run`) →
3. §1 C++ backends (`make check-all`) → 4. §4 Rust workspace →
5. §5 simulator → 6. §8 merge-integrity grep sweep →
7. §7 TLA+ (if TLC jar present) → 8. §6 gem5 (only if gem5 built).

Fastest signal for merge regressions is §8 (path sweep) + §1/§4/§5 (builds),
because the merge conflict risk is concentrated in path rewrites
(`expli_instr` → `explicit_api`) and the Rust/simulator files.