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
| Complete TX lifecycle | Every `TX_BEGIN` ends in `COMMIT_SUCCESS` or `TX_ABORT`; an end without a begin is CRITICAL |
| Lock discipline | Every `COMMIT_LOCK_ACQUIRE` has a matching `LOCK_RELEASE` |
| Abort causality | A `TX_ABORT` is preceded by a causal event (version/gap/lock failure) — WARNING: backends abort for reasons not all logged |
| No orphan locks | Locks held at the end of the log (or at a `SIGSEGV`) without release |
| Gap check outcome | Every `GAP_CHECK` is followed by `TX_ABORT` or a matching success path |
| Version ordering | Within a thread, read-set versions are ≤ the transaction's start version |
| Address validity | Event addresses are plausible (region/heap/stack classification) |
| Crash signature | Classifies the retained tail leading up to a `SIGSEGV` |
| No commit after abort | An aborted transaction does not later take the commit lock or publish |
| Memory lifecycle | Every `FREE` matches a live `MALLOC` of the same address; no double free |
| TX memory balance | Speculative allocations that survived a transaction are accounted for |
| Address reuse | A read/write on an address that was freed and not re-allocated (use-after-free) |
| Sequential baseline | Final value per address equals a `--baseline` single-threaded run (opt-in) |
| User oracle | `INVARIANT: … FAIL` lines the application printed are reported |

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

**Run it single-threaded** (`-T 1`) until the lifecycle and lock-discipline checks
pair events per thread id. They pair in one global stream, so when dumps from
concurrently running threads are interleaved — which is what `-T 4` and above
produces — they report false `CRITICAL` findings (measured: a TL2 run with all
driver oracles zero produced 93 of them across 52 dumps, while the same shape at
`-T 1` produced none). The checker says when a log contains more than one dump.


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
