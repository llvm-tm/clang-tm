# STM Bug Detection Tool

Fuzzing-based bug detection for STM backends using event-log tracing and
static invariant checking. Inspired by HawkSet (EuroSys 2025), Mumak
(EuroSys '23), and Harris & Jones's "Transactional Memory with Data
Invariants" (TRANSACT'06).

## Approach

1. **Fuzz**: Run a parameterized benchmark (counter array, bank transfers)
   with random thread counts, iteration counts, and data seeds.
2. **Trace**: The event logger (`#define TM_EVENT_LOG`) records every TM
   operation (TX begin/end/abort, lock acquire/release, read/write-set
   ops, commit phases) to a per-thread ring buffer.
3. **Check**: Post-hoc analysis validates static invariants against the
   event trace.  If an invariant is violated, the event log pinpoints
   which operation sequence caused the failure.
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

```bash
cd tools/stm_bug_tool

# Run fuzzing for all backends
python3 fuzz_runner.py --backends all --runs 10

# Run fuzzing for specific backend with 100 runs
python3 fuzz_runner.py --backends tl2,tinystm --runs 100

# Run single benchmark with event log visible
make -C benchmarks run BACKEND=tl2 THREADS=4

# Check event log from a previous run
python3 invariant_checker.py --log /tmp/event_log.txt
```

## Files

| File | Role |
|------|------|
| `fuzz_runner.py` | Orchestrates builds and runs across backends and parameters |
| `invariant_checker.py` | Invariant definitions and post-hoc checking on event traces |
| `event_parser.py` | Parses event log stderr output into structured data |
| `benchmarks/fuzz_counter.cpp` | Counter array fuzz target (generic TM correctness) |
| `benchmarks/fuzz_bank.cpp` | Bank transfer fuzz target (money conservation) |
| `benchmarks/Makefile` | Build rules for fuzz targets |
