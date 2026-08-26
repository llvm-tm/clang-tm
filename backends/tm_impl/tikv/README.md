# TiKV Distributed TM Backend

Demonstrates the expressive power of the TM abstraction:
**any distributed storage system can be wrapped with
transactional-memory semantics**.

## Architecture

```
TM abstraction  (tm_begin / tm_read / tm_write / tm_commit)
       │
       ▼
 TiKV backend   ───  TikvClient::begin_optimistic()
 (this crate)         txn.get(key)
                      txn.set(key, val)
                      txn.commit()   ← Percolator-style 2PC
       │
       ▼
 TiKV cluster   ───  PD + TiKV nodes (Raft replication)
```

- Each TM address maps to a TiKV key: `tm:{region_offset}`
- `tm_begin()` → TiKV `begin_optimistic()` (snapshot isolation)
- `tm_read()` → local write-set → TiKV `get()` (lazy-fetch, cached)
- `tm_write()` → buffer in local write-set
- `tm_commit()` → flush writes → TiKV `commit()` (2PC internally)
- `tm_abort()` → TiKV `rollback()`

TiKV provides the distributed fault tolerance: Raft consensus,
automatic failover, geo-replication. The TM abstraction layers
on top exactly as it would for any other storage system.

## Files

| File | Role |
|------|------|
| `runtime/tikv/src/lib.rs` | Rust backend (pure, uses `tikv-client` from crates.io) |
| `tikv_backend.cpp` | C++ hook shim linking to the Rust FFI |

The Rust crate is added as a Cargo dependency
(`tikv-client = "0.4"`), not vendored.

## Prerequisites

- A running TiKV cluster (≥ v5.0.0). Quick-start:
  ```sh
  git clone https://github.com/tikv/tikv
  cd tikv && cargo run --release -- tikv-server
  # Also start PD: https://tikv.org/docs/latest/deploy/
  ```
- Rust toolchain (for the crate dependency)
- Environment: `TM_TIKV_PD=127.0.0.1:2379` (PD endpoint)

## Building (Rust)

```sh
cd explicit_api/rust/workspace
cargo build --release -p bench-tinystm --features tikv
TM_TIKV_PD=127.0.0.1:2379 cargo run --release --features tikv --bin fuzz_counter
```

## Building (C++ with Rust FFI)

```sh
# 1. Build the Rust static library
cd backends/tm_impl/tikv && cargo build --release

# 2. Build C++ benchmark
cd benchmarks/cpp
make BACKEND=TIKV bank
TM_TIKV_PD=127.0.0.1:2379 ./bank -d 100 -a 16 -t 2
```

See `tikv_backend.cpp` for the Makefile integration snippet.

## Performance

Every TM read issues a **gRPC call** to TiKV (async via Tokio
`block_on`). Expect **1000–10000× slower** than shared-memory
backends. This is intentional — the point is to demonstrate
that the TM abstraction is **semantically complete**:
any storage system (KV store, message queue, SQL database)
can be wrapped, regardless of performance.

## Generalising the Pattern

The same approach works for any distributed storage:

- **Apache Kafka**: map TM addresses to Kafka partitions.
  `tm_begin()` creates a producer; `tm_read()` consumes from
  a compacted topic; `tm_commit()` produces a tombstone or
  snapshot record. (Very slow, but correct.)
- **Redis**: map TM addresses to Redis keys. Use WATCH/MULTI/EXEC
  for optimistic concurrency.
- **PostgreSQL**: map TM addresses to SQL rows. Use `SELECT ...
  FOR UPDATE` and `COMMIT`.

The TM API never changes. Only the backend implementation differs.
