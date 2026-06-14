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
# Run a bank scenario through a real backend
cargo run --bin tm-gen -- --scenario all | cargo run --bin tm-sim -- --backend norec

# Generate all scenarios to file and replay
cargo run --bin tm-gen -- --scenario all -o /tmp/trace.jsonl
cargo run --bin tm-sim -- --backend norec --trace /tmp/trace.jsonl
cargo run --bin tm-sim -- --backend tl2 --trace /tmp/trace.jsonl
cargo run --bin tm-sim -- --backend tinystm --trace /tmp/trace.jsonl

# Replay trace through the DES model
cargo run --bin tm-des -- --trace /tmp/trace.jsonl

# Check trace against a model
cargo run --bin tm-check -- /tmp/trace.jsonl

# Show checkpoint/restore
cargo run --bin tm-des -- --trace /tmp/trace.jsonl --checkpoint-every 10

# Convert a raw C++ trace to JSONL
cargo run --bin tm-trace2jsonl -- --input /tmp/raw.trc --output /tmp/trace.jsonl
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
{"ts":10,"tid":0,"seq":1,"kind":"TxBegin"}
{"ts":11,"tid":0,"seq":2,"kind":"Read","addr":"0x7f0000001000","width":4}
{"ts":12,"tid":0,"seq":3,"kind":"Write","addr":"0x7f0000001000","width":4,"val":0}
{"ts":13,"tid":0,"seq":4,"kind":"TxEnd"}
```

### Event kinds

| Kind | Fields | Description |
|------|--------|-------------|
| `TxBegin` | — | Start a transaction |
| `TxEnd` | — | Commit a transaction |
| `Read` | `addr`, `width` | Instrumented memory read |
| `Write` | `addr`, `width`, `val` | Instrumented memory write |
| `Log` | `msg` | Log message (scenario name) |
| `Checkpoint` | — | Save simulation state to disk |

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

## Running tests

```sh
cargo test                    # Unit + integration tests
cargo test -- --test-threads=1  # If concurrent NOrec tests conflict
cargo test --test sim_engine_test  # Integration tests only
```
