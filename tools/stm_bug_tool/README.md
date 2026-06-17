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

| # | Invariant | Description |
|---|-----------|-------------|
| 1 | **Complete TX lifecycle** | Every `TX_BEGIN` must be followed by either `COMMIT_SUCCESS` or `TX_ABORT` |
| 2 | **Lock discipline** | Every `COMMIT_LOCK_ACQUIRE` must have a matching `LOCK_RELEASE` |
| 3 | **Abort causality** | `READ_VERSION_CHECK` or `GAP_CHECK` failure must precede `TX_ABORT` |
| 4 | **No orphan locks** | All locks held at TX end are released |
| 5 | **Money conservation** | Sum of account balances is invariant across all TXs |
| 6 | **Counter monotonicity** | Counter increments equal expected per-thread total |
| 7 | **No lost updates** | Every write must have a valid read-set version ≤ write version |

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
