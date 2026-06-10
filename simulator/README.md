# TM Discrete Event Simulator

Replays TM traces through a software-TM model (WBCTL — word-based commit-time locking) for correctness checking and synchronization statistics.

## Tools

| Binary | Purpose |
|--------|---------|
| `tm-des` | Full DES engine with checkpoint/restore, live lock detection, opacity checks |
| `tm-check` | Fast trace replayer — validates and collects statistics (abort rates, contention, spin overhead, per-thread read/write set sizes) |
| `tm-gen` | Generates random TM traces for stress-testing |
| `tm-trace2jsonl` | Converts raw TM_TRACE_PATH format to JSONL |

## Usage

```sh
# End-to-end pipeline (build + run + convert + check)
./run_trace_pipeline.sh --backend NOREC --benchmark bank --duration 200 --threads 2

# Just check an existing trace
cargo run --bin tm-check -- /tmp/tm_pipeline/bank_NOREC.jsonl

# See statistics:
#   - Abort breakdown (read_spin_timeout, write_spin_timeout, commit_validate_fail, etc.)
#   - Synchronization overhead (validations, spins, lock contentions per transaction)
#   - Per-thread peak read-set / write-set sizes
```

## Statistics collected

The `tm-check` binary reports:

- **Abort rate** and breakdown by reason
- **Synchronization overhead** per transaction: validation count, snapshot extensions, spin iterations, lock contentions
- **Per-thread peak values**: read-set size, write-set size, total validations, total spins, extensions

These enable comparison across TM backends (NOREC vs TinySTM vs TL2 etc.) for the same workload.

## Trace format

Two formats:

1. **Raw** (`TM_TRACE_PATH`): compact one-event-per-line format produced by the C++ hook layer
2. **JSONL**: structured format consumed by the simulator. Each line is a JSON object with timestamp, thread-id, operation type, and optional data.
