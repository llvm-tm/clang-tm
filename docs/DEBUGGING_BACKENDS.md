# Debugging a TM Backend (tracing + deterministic replay)

Three built-in mechanisms help you find *why* a backend misbehaves (lost
updates, money not conserved, abort storms, hangs, corruption). Pick by symptom:

| Symptom | Use | Start at |
|---------|-----|----------|
| "Did my read/write actually get tracked? Are the read/write sets what I expect?" | **Event logger** (`-DTM_EVENT_LOG`) + a **debug patch** printing set sizes / bypass counts | §1, §2, [worked example](#worked-example-b-21-bank-tl2-money-not-conserved) |
| "A concurrency bug (lost update / race) reproduces only intermittently" | **Rust simulator** `tm-sim` — replay a trace through the real backend deterministically | §3 |
| "A load/store was silently left un-instrumented" | **IR / race-checker** (annotation pass, valgrind) | [INSTRUMENTATION_DEBUGGING.md](INSTRUMENTATION_DEBUGGING.md) |
| "Memory corruption / double free / leak" | `-DTM_DEBUG_ALLOC` + event logger `MALLOC/FREE` events | §1 + [EBR_DOUBLE_FREE_ANALYSIS.md](EBR_DOUBLE_FREE_ANALYSIS.md) |

All three are **already wired into the source** and compile to zero overhead
when off. Prefer them over ad-hoc `printf` in tracked files.

---

## 1. Event logger (`TM_EVENT_LOG`) — per-thread ring buffer

Header: `backends/tm_impl/common/tm_event_logger.hpp`. Backends already emit
`TM_EVENT(...)` / `TM_EVENT2(...)` at the key points (`WRITE_SET_INSERT`,
`READ_LOCK_ACQUIRE`, `COMMIT_LOCK_ACQUIRE`, `COMMIT_WRITEBACK`, `TX_ABORT`,
`MALLOC`, `FREE`, … — see the `EventType` enum in the header). When
`TM_EVENT_LOG` is undefined the macros expand to nothing.

```sh
# Build a backend/test with event logging on (example: a backend test)
make -C tests/backends clean all CXXFLAGS="-DTM_EVENT_LOG -UNDEBUG"
```

- **Auto-dump on crash:** a `SIGSEGV`/`SIGBUS` handler prints the last 512
  events for the faulting thread before exiting. Great for "which op corrupted
  the state".
- **Manual dump:** call `TM_EVENT_DUMP(N)` (from `tm_event_logger.hpp`) in the
  thread whose events you want (e.g. at the end of a worker, or in the
  backend's `tm_exit_thread`). Each thread has its own ring — dump per thread.

The logger records `addr1`/`addr2`/`data` per event, so you can confirm the
exact addresses reaching the tracking path (and their versions).

---

## 2. External debug patches (`patches/debug/`) — source stays clean

For invasive, one-off tracing (custom `fprintf`, counters, breakpoint traps)
that you do **not** want in git history, use the patch workflow in
[`../patches/debug/README.md`](../patches/debug/README.md):

```sh
# (a) hack the backend source with your debug code, then capture it:
git diff -- backends/... > patches/debug/patches/0NN-my-trace.patch
git checkout -- .                       # tree clean again
# (b) apply for a debugging session:
git apply patches/debug/patches/0NN-my-trace.patch        # ONE patch
#   or ./patches/debug/apply.sh to apply the whole set (sorted);
#   ./patches/debug/remove.sh to revert; ./patches/debug/status.sh to list.
# (c) build + run + read your output, then remove.sh when done.
```

> **Tip:** `apply.sh` applies *every* patch in `patches/debug/patches/`; if one
> is stale against current source it aborts the batch. To use a single example,
> `git apply patches/debug/patches/<NNN>.patch` (and `git apply -R …` to undo).

**Concrete example — `007-tl2-bypass-trace.patch`** adds counters to TL2 that
prove whether accesses reach the read/write-set code or are silently dropped by
the `isTMAddress` fast-path bypass:

```sh
git apply patches/debug/patches/007-tl2-bypass-trace.patch
make -C benchmarks/plugin/bank bank_tl2
./benchmarks/plugin/bank/bin/bank_tl2 -t 4 -d 300 -r 0 --test 2>&1 | grep "TL2 trace"
git apply -R patches/debug/patches/007-tl2-bypass-trace.patch
```

Other ready examples: `001-swisstm-commit-debug`, `002-tinystm-rs-ws-debug`
(max read/write-set sizes), `004-tm-region-alloc-debug`.

> **Prefer a pass over a patch for access tracing.** The `tm-access-trace` LLVM
> pass (`plugin/passes/TMAccessTracePass.cpp`, built as `bin/libTMAccessTrace.so`)
> injects a `tm_trace(read/write, addr, width, value)` event at every tracked
> access of *already-instrumented* IR, with no source patch:
> ```sh
> make -C plugin access-trace
> opt-22 -load-pass-plugin=plugin/bin/libTMAccessTrace.so -passes=tm-access-trace \
>      in.instr.bc -S -o out.ll
> make -C plugin test-access-trace   # IR + end-to-end test
> ```
> It reuses the existing sink (`backends/tm_impl/common/tm_trace_runtime.cpp`,
> `TM_TRACE_FILE=…`). The `007-tl2-bypass-trace` patch stays useful for
> *backend-internal* state (e.g. the `isTMAddress` bypass counts) that an IR pass
> cannot see. See [tm-access-trace-plan.md](tm-access-trace-plan.md).

---

## 3. Rust trace-replay simulator (`simulator/`) — deterministic concurrency

The simulator replays a **transaction trace** through the *real* backend model
(NOrec/TL2/TinySTM/SwissTM/Romulus/MVLog/TSX-SIM) under a **deterministic
schedule**, so an intermittent lost update becomes reproducible and inspectable.

```sh
cd simulator && cargo build --release

# Fastest start — synthetic bank scenarios, replay through each backend:
cargo run --release --bin tm-gen -- --scenario all -o /tmp/trace.jsonl
cargo run --release --bin tm-sim -- --backend tl2 --trace /tmp/trace.jsonl
cargo run --release --bin tm-sim -- --backend norec --trace /tmp/trace.jsonl   # compare

# Replay a trace captured from a REAL instrumented run instead of tm-gen:
TM_TRACE_PATH=/tmp/trc.jsonl ./some_instrumented_bench      # raw events
cargo run --release --bin tm-trace2jsonl -- --trace /tmp/trc.jsonl -o /tmp/trc.jsonl
cargo run --release --bin tm-check -- --trace /tmp/trc.jsonl  # model check + stats
```

Binaries: `tm-sim` (replay + verifier + deadlock detect), `tm-des` (discrete
event + checkpoint/restore + opacity), `tm-check` (fast model replayer),
`tm-gen` (synthetic bank traces), `tm-trace2jsonl` (raw→JSONL). Details in
[`../simulator/README.md`](../simulator/README.md); fidelity notes in
[`../simulator/ASSESSMENT.md`](../simulator/ASSESSMENT.md). Use
`--clock-mode cost` for contention-aware throughput.

Reproducing a bug deterministically: capture a `TM_TRACE_PATH` trace from the
failing native run, then replay through `tm-sim` for the suspect backend; if the
sim reproduces the corruption, the bug is in the backend's read/write-set /
validation logic (not in the platform memory model); if it does **not**, suspect
memory-ordering / fences.

---

## Worked example — B-21: bank `tl2` money not conserved

Symptom: `bank_tl2 -t 4 -d 600 -r 0 --test` (100% transfers) drifted by ~-400
while `bank_norec`/`bank_tinystm` conserved money.

1. **Count aborts** (patch a `tm_exit()` `fprintf` of the commit-fail counter):
   `commit_fail = 0` under 10M+ transactions ⇒ the OCC validation never fired.
2. **Trace the read/write sets** with `007-tl2-bypass-trace.patch` (§2). Output:
   ```
   commits=2621821  tracked_write=0  max_ws=0
   bypass_write=5243642  bypass_read=10487284  tracked_read=5243642
   ```
   `tracked_write=0` / `max_ws=0`: **no account write ever entered the
   write-set.** The two balance writes/commit went to `bypass_write`.
3. **Root cause:** `TMSafeVector` (the bank's `accounts`) allocates with
   `::operator new` (`backends/tm_impl/common/tm_vector.hpp:27`), so its data is
   on the **plain heap**, outside the mmap'd TM region. TL2/SwissTM skip such
   addresses via the `isTMAddress` fast-path bypass (`tl2.hpp` read/write
   `impl`), so shared balances are never tracked. NOrec/TinySTM lack that
   address filter (they track/serialize everything), which is why they passed.
   (The only tracked accesses were the `bank` static-global pointer reads — the
   B-30 `isTMGlobal` registration.)

This is the canonical "untracked-shared-data-behind-the-region-bypass" failure:
any transactional data that is **not** in the TM region and **not** a registered
static global is invisible to address-filtered backends. See
[CORRECTNESS_FIXES.md](CORRECTNESS_FIXES.md).

---

## See also

- [INSTRUMENTATION_DEBUGGING.md](INSTRUMENTATION_DEBUGGING.md) — missing-instrumentation
  (IR inspection, `-tm-audit`, valgrind/helgrind/drd).
- [proofs.md](proofs.md) — TLA+/TLC models; `TL2RMW.tla` machine-checks TL2
  serializability (design is sound → look for an *implementation* deviation).
- [plugin-debug.md](plugin-debug.md) — GDB + plugin test binaries.
- [`../patches/debug/README.md`](../patches/debug/README.md) — patch workflow detail.
- [`../simulator/README.md`](../simulator/README.md) — simulator detail.
