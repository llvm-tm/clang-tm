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

## 4. ThreadSanitizer sweeps (`SANITIZE=thread`)

Every backend builds and runs under `-fsanitize=thread`; the vehicle is the T1
differential driver, whose oracles are self-verifying:

```sh
make -C benchmarks/cpp bin/drv_diff_ops BACKEND=TL2 SANITIZE=thread
TSAN_OPTIONS="halt_on_error=0:external_symbolizer_path=" \
  ./benchmarks/cpp/bin/drv_diff_ops -S 1 -T 4 -n 200
```

Practical notes, each of which cost real time at least once:

- **`external_symbolizer_path=` is required here.** TSan forks a symbolizer, and
  the one that pairs with LLVM 22 dies inside `llvm-symbolizer` ("CachedFileStream
  was not committed" / `Can't read from symbolizer at fd 3`), which silently
  turns a useful report into an unsymbolized one. Run it with no external
  symbolizer and resolve the offsets yourself, below.
- **Resolve frames against the exact binary that produced the report**, using the
  module-relative offset TSan prints:

  ```sh
  addr2line -i -C -f -e benchmarks/cpp/bin/drv_diff_ops 0xf4769
  ```

  `-i` matters: the interesting frame is usually inlined into `commit()` or a
  `read_word()`.
- **Data addresses need the load base.** For the *accessed object* TSan prints an
  absolute runtime address. Subtract the PIE load base (first mapping of the
  binary in `/proc/<pid>/maps`, easy to catch by running the binary with a large
  `-n` and sampling it), then look the resulting link-time address up in
  `nm -S --defined-only` — that is how `tm_rtm::available()::cached` was
  identified in §31 of [CORRECTNESS_FIXES.md](CORRECTNESS_FIXES.md).
- **Copy the binary per backend when sweeping.** `make … BACKEND=X` overwrites
  `benchmarks/cpp/bin/drv_diff_ops`, so symbolizing afterwards resolves every
  backend's offsets against the *last* one built and yields confident, completely
  wrong function names. Snapshot as you go (`cp bin/drv_diff_ops /tmp/bin-X`).
- **Exit codes are the gate.** `drv_diff_ops` exits 2 on any consistency
  violation, 0 otherwise, so `ci.yml`'s `tsan-gate` job can fail on the process
  status and count warnings from the log.
- A race between `commit`'s write-back and a lockless speculative read is the
  *intended* protocol in the single-version backends; it is expressed with
  relaxed loads and release stores since
  [CORRECTNESS_FIXES.md §30](CORRECTNESS_FIXES.md). A fresh report of that shape
  means plain accesses crept back in.
- The driver's `mersenne_twister_engine` frames are **not** a race: each worker
  thread owns its RNG. If you see that name, you are looking at mis-symbolization
  (previous bullet).

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

**Cross-implementation scope (it is *not* a protocol bug).** Running the same
`bank` scenario through all three pipelines with `tl2`:

| Pipeline | Result | Why |
|---|---|---|
| LLVM plugin (`bank_tl2`) | drifts | `TMSafeVector` → `::operator new` → heap → bypassed |
| C++ explicit API (`benchmarks/cpp/bank -t N`, `BACKEND=TL2`) | **conserves** | `expli::vector` → `tm_malloc` → in-region → tracked |
| Rust simulator (`tm-sim --backend tl2`) | **conserves** | tracks every address |

So `TL2RMW.tla` is right: the OCC protocol is serializable in all three. The
divergence is an **allocation / address-tracking contract**, not an interleaving,
so it can't be caught by a concurrency model — only by a runtime contract check.

### Fail-loud guard (`TM_STRICT_ADDR=1`)

That contract is now enforced (opt-in) in `tl2.hpp` / `SwissTM.hpp`: when the
plugin instruments an access *as transactional* but the address is neither in the
TM region nor a registered global, set `TM_STRICT_ADDR=1` to **abort loudly**
(`TM-ADDR-CONTRACT: transactional read/write to UNTRACKED address …`) instead of
silently corrupting. Default stays OFF (bypass) so existing benchmarks are
unaffected; `TM_ALLOW_UNTRACKED=1` keeps bypass explicitly.

```sh
make -C plugin test-strict-addr        # regression: default=bypass, strict=abort
TM_STRICT_ADDR=1 ./benchmarks/plugin/bank/bin/bank_tl2 -t 4 -d 200 --test   # now aborts on the heap access
```
The fix is then mechanical: allocate the offending container from the TM region
(like `expli::vector`) so the access becomes tracked.

---

## See also

- [INSTRUMENTATION_DEBUGGING.md](INSTRUMENTATION_DEBUGGING.md) — missing-instrumentation
  (IR inspection, `-tm-audit`, valgrind/helgrind/drd).
- [proofs.md](proofs.md) — TLA+/TLC models; `TL2RMW.tla` machine-checks TL2
  serializability (design is sound → look for an *implementation* deviation).
- [plugin-debug.md](plugin-debug.md) — GDB + plugin test binaries.
- [`../patches/debug/README.md`](../patches/debug/README.md) — patch workflow detail.
- [`../simulator/README.md`](../simulator/README.md) — simulator detail.
