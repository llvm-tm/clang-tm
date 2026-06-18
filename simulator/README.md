# TM Discrete Event Simulator

Replays TM traces through both a software-TM model and real TM backends (NOrec, TL2, TinySTM) for correctness checking and synchronization statistics.

## Tools

| Binary | Purpose |
|--------|---------|
| `tm-sim` | Replays traces through a **real TM backend** (NOrec/TL2/TinySTM) with verifier and deadlock detection |
| `tm-des` | Full discrete-event simulation engine with checkpoint/restore, live lock detection, opacity checks |
| `tm-check` | Fast trace replayer — validates against a model and collects statistics |
| `tm-gen` | Generates TM bank-scenario traces for validation |
| `tm-trace2jsonl` | Converts raw `TM_TRACE_PATH` format to JSONL |

## Usage

```sh
# Build first (all five binaries)
cargo build --release

# Generate all scenarios to file and replay
cargo run --release --bin tm-gen -- --scenario all -o /tmp/trace.jsonl
cargo run --release --bin tm-sim -- --backend norec --trace /tmp/trace.jsonl
cargo run --release --bin tm-sim -- --backend tl2 --trace /tmp/trace.jsonl
cargo run --release --bin tm-sim -- --backend tinystm --trace /tmp/trace.jsonl

# Replay trace through the DES model
cargo run --release --bin tm-des -- --trace /tmp/trace.jsonl

# Check trace against a model
cargo run --release --bin tm-check -- --trace /tmp/trace.jsonl

# Validate a pre-built trace
cargo run --release --bin tm-check -- --trace traces/bank_lost_update.jsonl

# Show checkpoint/restore
cargo run --release --bin tm-des -- --trace /tmp/trace.jsonl --checkpoint-every 10

# Run a specific scenario
cargo run --release --bin tm-gen -- --scenario lost-update -o /tmp/lu.jsonl
cargo run --release --bin tm-sim -- --backend tl2 --trace /tmp/lu.jsonl

# Convert a raw C++ trace to JSONL
cargo run --release --bin tm-trace2jsonl -- --input /tmp/raw.trc --output /tmp/trace.jsonl
```

## Architecture

```
                   ┌─────────────┐
                   │   tm-gen    │  Generates bank-scenario traces
                   └──────┬──────┘
                          │ JSONL
                          ▼
              ┌───────────┴───────────┐
              │      Trace file       │
              │     (JSONL format)    │
              └───────────┬───────────┘
                          │
              ┌───────────┴───────────┐
              │      tm-sim / tm-des  │
              │                       │
              │  ┌─────────────────┐  │
              │  │   Event Queue   │  │  Timestamp-ordered execution
              │  └────────┬────────┘  │
              │           ▼           │
              │  ┌─────────────────┐  │
              │  │  SimEngine      │  │  Dispatches to real backend
              │  ├─────────────────┤  │
              │  │  Verifier       │  │  Shadow memory + invariant checks
              │  ├─────────────────┤  │
              │  │ DeadlockDetector│  │  Wait-graph DFS cycle detection
              │  └─────────────────┘  │
              └───────────────────────┘
```

## Trace format

JSONL — one JSON object per line:

```json
{"timestamp":10,"thread_id":0,"seq":1,"kind":"TxBegin"}
{"timestamp":11,"thread_id":0,"seq":2,"kind":{"Read":{"addr":139637976731648,"width":4}}}
{"timestamp":12,"thread_id":0,"seq":3,"kind":{"Write":{"addr":139637976731648,"width":4,"val":0}}}
{"timestamp":13,"thread_id":0,"seq":4,"kind":"TxEnd"}
```

Note: addresses are serialized as raw integers via `serde_json`. The hex address `0x7f0000001000` corresponds to integer `139637976731648`.

### Event kinds

| Kind | Fields | Description |
|------|--------|-------------|
| `TxBegin` | — | Start a transaction |
| `TxEnd` | — | Commit a transaction |
| `Read` | `addr`, `width` | Instrumented memory read |
| `Write` | `addr`, `width`, `val` | Instrumented memory write |
| `Alloc` | `addr`, `size` | TM memory allocation |
| `Free` | `addr` | TM memory free |
| `Log` | `msg` | Log message (scenario name) |
| `Checkpoint` | — | Save simulation state to disk |
| `Assert` | `cond`, `msg` | Check a boolean condition; fails if `cond` is false |
| `ThreadSpawn` | `child_id` | Spawn a new simulated thread |
| `ThreadJoin` | `child_id` | Join a simulated thread |

## Scenario-based validation (`tm gen`)

`tm-gen` generates parameterized bank-transfer scenarios. Each scenario tests a specific TM isolation property:

| Scenario | What it tests | Expected outcome |
|----------|--------------|------------------|
| `simple` | Single-threaded transfer A→B | 1 commit, no aborts, money conserved |
| `scan` | Read-only scan of 4 accounts | 1 commit, consistent snapshot |
| `conflict` | Two threads transferring same account | Both succeed (same value written) or 1 aborts |
| `disjoint` | Two threads, disjoint accounts | Both commit, money conserved |
| `lost-update` | Write-after-read conflict (classic lost update) | 1 commit + 1 abort |
| `write-skew` | Read-set staleness (snapshot isolation anomaly) | Should abort for serializable backends |

Money conservation is verified across all scenarios by the `Verifier`.

## Verifier checks

The `tm-sim` binary includes a full verifier (`verifier.rs`) that tracks shadow memory and checks:

- **Double-free**: same address freed twice without intervening allocation
- **Use-after-free**: memory access after free
- **Money conservation**: sum of bank balances is invariant across transactions
- **Memory leak**: unfreed allocations at end of trace

## Raw trace format (`tm-trace2jsonl`)

The raw C++ `TM_TRACE_PATH` format is whitespace-delimited, one event per line:

| Column | Type | Description |
|--------|------|-------------|
| 1 | `u64` | Timestamp |
| 2 | `u32` | Thread ID |
| 3 | `u32` | Type code (0=Read, 1=Write, 2=TxBegin, 3=TxEnd, 4=Alloc, 5=Free) |
| 4 | `u64` | Address (hex, e.g., `0x7f0000001000`) |
| 5 | `u64` | Width in bytes (Read/Write) or allocation size (Alloc). Default: 8 |
| 6 | `u64` | Value (Write only, hex). Default: 0 |

Lines starting with `#` are comments. Type codes > 5 emit a warning and are skipped.

```
# Simple transfer trace
10 0 2 0          # TxBegin on thread 0 at ts=10
11 0 0 0x1000 4   # Read 4 bytes from 0x1000
12 0 1 0x1000 4 0 # Write 0 (4 bytes) to 0x1000
13 0 3 0          # TxEnd (commit)
```

## Deadlock detector (P6)

`tm-sim` includes a deadlock/livelock detector (`deadlock.rs`):

- Builds a **wait-for conflict graph** from write-set overlaps between concurrent transactions
- Runs **DFS cycle detection** to find deadlocks
- Tracks per-thread retry counts and emits warnings on livelock threshold
- Reports detected deadlock cycles in simulation output

## Checkpoint/restore (P7)

Both `tm-sim` and `tm-des` support saving and restoring simulation state:

- `Checkpoint` events in the trace trigger a save
- `tm-des` supports `--checkpoint-every N` for periodic saves
- Backend state is captured as opaque blobs (`sim_snapshot_bytes` / `sim_restore_bytes`)
- Full engine state (SIM engine + backend) is serialized via bincode
- Enables "replay from checkpoint" debugging

## Real backend support

`tm-sim` dispatches trace events to real TM backends:

| Backend | Validation | Locking | Feature flag |
|---------|-----------|---------|-------------|
| NOrec | Value-based | Global versioned lock | `sim-backend-norec` |
| TL2 | Version-based (commit-time locking) | Per-object hash locks | `sim-backend-tl2` |
| TinySTM | Encounter-time locking | Per-object hash locks | `sim-backend-tinystm` |

Simulation mode uses a thread-ID multiplexing layer (`sim_tx_store`) instead of `thread_local!`, enabling multiple simulated threads to run on a single OS thread.

## Rust vs C++ backend comparison

The Rust backends (under `runtime-norec`, `runtime-tl2`, `runtime-tinystm`) are reimplementations of the C++ backends (under `backends/tm_impl/`). The simulator enables direct comparison:

- Same trace + same backend algorithm → should produce same commit/abort behavior
- Any divergence indicates a bug in Rust vs C++ translation
- Bank scenarios provide deterministic, reproducible test vectors

## Known limitations

- **Global static state**: The Rust TM backends use `static` variables (`GLOBAL_LOCK`, `COMMIT_LOCK`, `G_CLOCK`). Running multiple `SimEngine` instances in the same process causes cross-contamination. Use `--test-threads=1` for test isolation. This is a fundamental limitation of the backend architecture.
- **Linux-only mmap**: The TM address space is mapped at `0x7f00_0000_0000` via `MAP_FIXED`. This address is Linux x86_64-specific. macOS/Windows will likely fail. The C++ backends have the same limitation.
- **Exact-address verification**: The verifier tracks allocations by base address only. Out-of-bounds or sub-offset accesses (e.g., `addr+4` within a 64-byte allocation) are not detected. Range-based verification would require an interval tree data structure.
- **Money conservation**: Requires `--initial-values` to be specified (a JSON file mapping hex addresses to their initial values). Without it, the check is skipped. The bank scenarios in this repository do not embed initial values in the trace format.
- **tm-des.output.jsonl**: The output file contains processed events in timestamp order. It is a debugging aid, not a stable output format. Field names and serialization may change.
- **Trace determinism**: The simulation is fully deterministic from the trace data alone. The `--seed` parameter was removed as dead code (the PRNG in `LpState` was never used).

## Running tests

```sh
cargo test                    # Unit + integration tests
cargo test -- --test-threads=1  # If concurrent NOrec tests conflict
cargo test --test sim_engine_test  # Integration tests only
```
