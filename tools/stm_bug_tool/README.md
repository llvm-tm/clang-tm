# STM Bug Detection Tool

Fuzzing-based bug detection for STM backends using event-log tracing and
static invariant checking. Inspired by HawkSet (EuroSys 2025), Mumak
(EuroSys '23), and Harris & Jones's "Transactional Memory with Data
Invariants" (TRANSACT'06).

## Approach

1. **Fuzz**: Run a parameterized benchmark (counter array, bank transfers)
   with random thread counts, iteration counts, and data seeds.
2. **Trace**: Trace events (TX begin/end, reads, writes, aborts) are logged
   via the instrument pass (`--emit-tm-trace`) or runtime wrappers
   (`TM_TRACE_PATH` / `TM_TRACE_FILE`).
3. **Check**: Post-hoc analysis validates static invariants against the
   trace.  If an invariant is violated, the trace pinpoints which operation
   sequence caused the failure.
4. **Report**: FAIL per (backend, fuzz_params) with the violating event
   sequence.

## Invariants

The implemented checks (`ALL_INVARIANTS` in `invariant_checker.py`), with the
severity each violation is reported at:

| Invariant | What it enforces |
|-----------|------------------|
| Complete TX lifecycle | Per thread id: every `TX_BEGIN` ends in `COMMIT_SUCCESS` or `TX_ABORT`; an end without a begin is CRITICAL |
| Lock discipline | Per thread id: every `COMMIT_LOCK_ACQUIRE` has a matching `LOCK_RELEASE` from the same thread |
| Abort causality | Per thread id: a `TX_ABORT` is preceded, on the same thread, by a causal event (`READ_VERSION_CHECK`, `GAP_CHECK`, `WRITE_LOCK_ACQUIRE`) — WARNING: backends still abort for reasons not all logged (TL2's two commit routes now log theirs, `docs/CORRECTNESS_FIXES.md` §42) |
| No orphan locks | Locks held at the end of the log (or at a `SIGSEGV`) without release |
| Gap check outcome | Every `GAP_CHECK` is followed by `TX_ABORT` or a matching success path |
| Version ordering | Within a thread, read-set versions are ≤ the transaction's start version — a `WARNING`: it encodes the versioned-lock discipline, and value-validated backends (NOrec, NORECBF, TL2) break it by design (`docs/CORRECTNESS_FIXES.md` §40) |
| Address validity | Event addresses are plausible (region/heap/stack classification) |
| Crash signature | Classifies the retained tail leading up to a `SIGSEGV` |
| No commit after abort | An aborted transaction does not later take the commit lock or publish |
| Memory lifecycle | Every `FREE` matches a live `MALLOC` of the same address; no double free |
| TX memory balance | Speculative allocations that survived a transaction are accounted for |
| Address reuse | A read/write on an address that was freed and not re-allocated (use-after-free) |
| Sequential baseline | Final value per address equals a `--baseline` single-threaded run (opt-in) |
| User oracle | `INVARIANT: … FAIL` **and** `TM-INVARIANT: FAIL` verdict lines the program printed — the fuzz programs and the `tm_register_invariant_callback()` wrapper (`tm_hooks.cpp:831`) — are reported. It read a key nothing ever set, so it had never fired for any log until 2026-11-26 — `docs/CORRECTNESS_FIXES.md` §41, including the correction to its first fix |

CRITICAL findings make the exit status 1; WARNING findings do not unless
`--strict` was given.

## Running the checker

```bash
python3 tools/stm_bug_tool/invariant_checker.py --log /tmp/event_log.txt
```

Options: `--baseline FILE` (`0xaddr 0xvalue` per line, from a single-threaded
run) feeds the *Sequential baseline* invariant; `--events lifecycle,orphan`
runs a subset (substring match on the names above); `--quiet` prints only the
summary; `--strict` also fails on warnings. Exit status: 0 clean, 1 on any
CRITICAL, 2 on warnings under `--strict`, 2 on a bad `--events` filter.

**The ring is per thread and finite.** `TM_EVENT_DUMP` dumps the *calling*
thread's buffer, so a multi-threaded program produces one dump per thread that
calls it, and `dump_start > 0` means earlier events were overwritten — the
checker says so, because lifecycle and memory-lifecycle results on a wrapped log
are weaker than on a complete one.

### Producing a log

Events are compiled in only with `-DTM_EVENT_LOG`
(`backends/tm_impl/common/tm_event_logger.hpp`); without it the `TM_EVENT`
macros expand to nothing. A dump then has to be requested: either from the
`SIGSEGV`/`SIGBUS` handler (which prints the last 512 events of the faulting
thread) or explicitly with `TM_EVENT_DUMP(n)` — currently only the differential
driver does the latter, under `DRV_EVENT_DUMP=1`:

```bash
make -B -C benchmarks/cpp bin/drv_diff_ops BACKEND=TL2 EXTRA_CXXFLAGS=-DTM_EVENT_LOG
DRV_EVENT_DUMP=1 ./benchmarks/cpp/bin/drv_diff_ops -S 1 -T 1 -n 300 2>/tmp/event_log.txt
python3 tools/stm_bug_tool/invariant_checker.py --log /tmp/event_log.txt
```

Note the `2>`: the logger writes to stderr, so the log and the program's own
stdout must not be conflated. `EXTRA_CXXFLAGS` adds flags without replacing the
Makefile's own (`CXXFLAGS=` would drop the `-I` paths).

**Multi-worker logs are usable for the two pairing checks.** `Complete TX
lifecycle` and `Lock discipline` pair events **per thread id**. They used to pair
in one global stream, so a log with dumps from concurrently running threads
interleaved them and reported false `CRITICAL` findings (measured: 93 across 52
dumps on a TL2 `-T 4` run whose driver oracles were all zero, 0 on the same shape
at `-T 1`). Pairing per thread id takes that to 0 for lock discipline, and the
remaining findings were then traceable to something else — see the next paragraph.
The checker still tells you when a log holds more than one dump, and per-dump wrap
state is tracked for every dump in the file, not just the last one.

**What a lifecycle finding really means.** Those two checks assert what the *log*
says, so a backend that does not log an abort produces a `TX_BEGIN` with no end,
which is indistinguishable from a lost transaction. TL2's `commit()` logged nothing
on its two conflict-abort routes (guard-acquire failure and read-set validation
failure), so a contended TL2 run reported 130 and 317 lifecycle criticals at two
shapes with zero real violations, and 0 `TX_ABORT` events in 19 210 events of a log
in which every other aborting backend logs them. Fixed in
`docs/CORRECTNESS_FIXES.md` §38, which also records the A/B (130, 317 and 372 → 0 at
identical commit counts). Before trusting a lifecycle `CRITICAL` from an unfamiliar
backend, count its `TX_ABORT` events against the driver's own `aborts=` line.

**What `CRITICAL` means here.** A `-T 4 -n 300` NOrec log (built the same way) once
produced 3 427 `CRITICAL`s. 1 246 lock-discipline and 1 250 address-validity of them were
NOrec logging its commit-lock *version* where the address belongs, and never logging the
unlock — `docs/CORRECTNESS_FIXES.md` §39 fixes both to 0. The remaining 1 371 were
`Version ordering`, which turned out to be the check being wrong rather than the backend:
NOrec advances `tx->snapshot` only through a `validate()` that re-reads and value-checks
the entire read set first, so its newer-version reads are safe by construction — measured
in §40, where the value-check abort count equalled the driver's `aborts=` in every shape
and all four consistency oracles stayed at 0. That check is now a `WARNING`. All three
real logs measured afterwards are `0 critical` (NOrec 1 422 warnings, NORECBF 2 128, the
§38 TL2 log 386 — §42's stricter `abort_causality` scan moved those totals up from
1 416 / 2 119 / 352, the critical counts unchanged) while the deliberately-broken fixture
still reports 3, so the `CRITICAL` class stays meaningful and `--strict` still distinguishes
clean from not.

What `abort_causality` still means: TL2's two commit-path conflict routes were logging a
bare `TX_ABORT`, and the check itself paired one global event stream so it credited another
thread's conflict as the explanation — both fixed in `docs/CORRECTNESS_FIXES.md` §42, which
measures the pair (a TL2 run whose 259 and 282 aborts all carry their own reason now reports
**0**, where the old backend plus the old check reported 122 and 45). The scan is stricter
than it was, so counts went *up* on logs from backends that still abort silently: NOrec 41 →
47, NORECBF 84 → 93, the §38 TL2 log 316 → 350. Those are real gaps in those backends'
traces, listed in `TODO.md`, along with the `Version ordering` work needed to make it
backend-aware rather than merely a warning. So the checker remains a diagnostic tool, not a
gate on backend behaviour.

## Testing the checker itself

`selftest.py` holds hand-written traces — one per defect, nothing else in them — and the
exact count and severity each must produce, plus the CLI's exit codes:

```bash
make check-pytools          # == python3 tools/stm_bug_tool/selftest.py
```

It runs in CI (the `lint` job), because the alternative is worse: the checker is not
otherwise gated, so a `CRITICAL` becoming a `WARNING`, or an invariant quietly losing the
input it reads, would show up as a green build and a tool that says nothing is wrong for a
month. Two guards in it are worth knowing about: the `clean` fixture must produce **no**
findings at all, and at least one fixture must produce a `CRITICAL` — a suite that never
says "bad" is as useless as one that never says "good". `stale_read` pins §40's demotion
specifically, so re-promoting `Version ordering` has to be an edit to that file.


## Usage

### New fuzz driver (LLVM plugin pipeline)

```bash
# Basic: fuzz a benchmark with TINYSTM
python3 tools/tm-fuzz/tm-fuzz.py --app benchmarks/plugin/bank/bank.cpp --backend TINYSTM

# With strategy pass and multiple thread counts
python3 tools/tm-fuzz/tm-fuzz.py --app benchmarks/plugin/bank/bank.cpp \
    --strategy auto --threads 1,2,4 --duration 10

# With trace and invariant checking
python3 tools/tm-fuzz/tm-fuzz.py --app benchmarks/plugin/bank/bank.cpp \
    --trace --baseline baseline.txt --duration 30

# All backends
python3 tools/tm-fuzz/tm-fuzz.py --app benchmarks/plugin/bank/bank.cpp --backend all
```

### Manual event log checking

```bash
cd tools/stm_bug_tool

# Check event log from a previous run
python3 invariant_checker.py --log /tmp/event_log.txt
```

## Files

| File | Role |
|------|------|
| `invariant_checker.py` | Invariant definitions and post-hoc checking on event traces |
| `event_parser.py` | Parses event log stderr output into structured data |
| `tools/tm-fuzz/tm-fuzz.py` | Generic TM fuzz driver (LLVM plugin pipeline) |

## See Also

- `docs/fuzz-tool-plan.md` — Design plan and implementation status for the fuzz tool
- `plugin/passes/TMFuzzStrategyPass.cpp` — Strategic point detection pass
- `backends/tm_impl/common/tm_trace_runtime.cpp` — Runtime trace collector
