# TM Discrete Event Simulator — Plan

## 1. Why a DES?

Transactional memory implementations have correctness proofs (see `docs/proofs.md`), but real C++ implementations have bugs: deadlock, livelock, opacity violations, double-frees, and TM-address-space violations. A DES lets us verify TM behavior at the event level without running on real hardware, replaying traces from both the fuzz tester and the LLVM plugin.

---

## 2. Architecture

```
┌─────────────────────┐    trace file     ┌──────────────────────┐
│  LLVM Plugin        │ ──────────────→   │  DES Engine          │
│  (cleanup pass)     │   events.jsonl    │  (Rust binary)       │
│                     │                   │                      │
│  --emit-tm-trace    │                   │  ┌────────────────┐  │
│                     │                   │  │ Event Queue    │  │
│  fuzz_tool          │                   │  │ (BinaryHeap)   │  │
│  (generates .cpp)   │                   │  └────────────────┘  │
└─────────────────────┘                   │  ┌────────────────┐  │
                                          │  │ Logical Procs │  │
┌─────────────────────┐                   │  │ (threads + TM) │  │
│  Rust rng           │                   │  └────────────────┘  │
│  (checkpointable)   │                   │  ┌────────────────┐  │
└─────────────────────┘                   │  │ Checkpointer  │  │
                                          │  └────────────────┘  │
                                          └──────────────────────┘
```

### 2.1 Event Queue

- **Data structure**: `BinaryHeap<(Timestamp, Event)>` (max-heap → invert for min-heap).
- This is the textbook choice for DES: O(log n) insertion, O(1) peek, O(log n) pop.
- Alternatives considered: `Vec` + sort (O(n log n) per insertion), skip list (O(log n) but more complex). Binary heap wins for simplicity and performance.
- Events are ordered by ascending timestamp.
- Tie-breaking: thread ID then a global insertion counter.

```rust
struct Event {
    timestamp: u64,
    thread_id: u32,
    seq:       u64,           // global insertion order tie-breaker
    kind:      EventKind,
}

enum EventKind {
    ThreadSpawn(u32),          // (child_id)
    TxBegin,
    TxEnd,
    Read { addr: u64, width: u8 },
    Write { addr: u64, width: u8, val: u64 },
    Alloc { size: u64 },
    Free { addr: u64 },
    Checkpoint,
    Assert { cond: bool, msg: String },
}
```

### 2.2 Logical Processes

Each thread is a logical process (LP). The LP models:
- **Non-transactional** code blocks (direct read/write — no logging).
- **Transactional** code blocks (read/write through TM — write-set/read-set logging).
- **TM internals**: orec/lock acquire/release, version-clock reads, commit/abort logic.
- **Memory allocation**: region allocator vs heap, deferred-free lists, spec-alloc lists.

The LP has its own PRNG state (checkpointable) so that replay is deterministic.

**TM model** (simplified, STM-specific):
```
state = IDLE | ACTIVE | COMMITTING | ABORTING

begin():
    state = ACTIVE
    start_version = g_clock.load()
    clear_spec_allocs()
    clear_deferred_frees()

read(addr):
    if state == ACTIVE:
        lock = locate_orec(addr)
        lock.acquire_read()
        add_to_read_set(addr, lock.version)
        return mem[addr]

write(addr, val):
    if state == ACTIVE:
        lock = locate_orec(addr)
        lock.acquire_write(tx_id)   // detect deadlock via timeout
        add_to_write_set(addr, old_val, new_val)
        log_undo(addr, old_val)
        mem[addr] = val

commit():
    validate_read_set()     // check all orec versions unchanged
    lock_write_set()        // acquire all write locks
    g_clock++
    release_all()
    flush_spec_allocs()

abort():
    undo_write_set()
    release_all()
    clear_spec_allocs()
    state = IDLE
```

**Deadlock detection**: A write-lock acquire that spins beyond a configurable threshold triggers a deadlock event. The simulator logs the cycle.  
**Livelock detection**: A transaction that retries N times (configurable) without making progress triggers a livelock warning.

### 2.3 Fast Checkpointable PRNG

Use the `rand_chacha` crate (ChaCha12 is fast and cryptographically strong, but for DES we only need statistical quality). The key feature: the PRNG state implements `Serialize`/`Deserialize` so we can snapshot and restore.

```rust
use rand_chacha::ChaChaRng;
use rand::{RngCore, SeedableRng};
use serde::{Serialize, Deserialize};

#[derive(Serialize, Deserialize)]
struct CheckpointableRng {
    rng: ChaChaRng,
}

impl CheckpointableRng {
    fn save(&self) -> Vec<u8> { bincode::serialize(&self).unwrap() }
    fn load(data: &[u8]) -> Self { bincode::deserialize(data).unwrap() }
}
```

Every LP gets its own `CheckpointableRng` seeded from a global seed + thread ID.

### 2.4 Checkpoint / Restore

The simulation state at a given timestamp is a snapshot of:
- Event queue (remaining future events)
- All LP states (registers, TM state, PRNG)
- Global memory
- TM global data (clock, lock table)
- Speculative allocation tracking

Checkpoints are stored as serialized blobs (bincode). On restore, we:
1. Deserialize the checkpoint.
2. Rebuild the event queue.
3. Continue processing from the checkpoint timestamp.

Checkpoints happen:
- Periodically (every N events, configurable).
- On explicit `Checkpoint` events in the trace.
- Before fuzz-replay.

---

## 3. LLVM Plugin Integration

### 3.1 Trace Emission (`--emit-tm-trace`)

Add a flag to the LLVM plugin's cleanup pass. When set, the pass emits a JSONL trace file before exiting:

```
--emit-tm-trace=/tmp/my_trace.jsonl
```

The trace contains one JSON object per line:

```jsonl
{"ts":0,    "tid":0, "op":"tx_begin"}
{"ts":1,    "tid":0, "op":"write", "addr":"0x7f00a000", "width":8, "val":42}
{"ts":2,    "tid":0, "op":"read",  "addr":"0x7f00a008", "width":8}
{"ts":3,    "tid":0, "op":"tx_end"}
```

For plugin-instrumented code, the pass records every `tm_begin`/`tm_end`/`tm_read_*`/`tm_write_*`/`tm_malloc`/`tm_free` call. The timestamp is a synthetic counter (instruction count or sequential event number).

Thread spawn/join events are also recorded so the simulator knows the thread topology.

### 3.2 Linking Events to Source

The trace includes DWARF debug info (file:line) for each event, so failing simulations can be mapped back to the source:

```jsonl
{"ts":4, "tid":1, "op":"read", "addr":"0x7f00a000", "width":4, "file":"bank.cpp", "line":142}
```

---

## 4. Simulation Verifications

### 4.1 Non-transactional vs Transactional Code Blocks

The trace marks non-transactional code sections explicitly. The simulator checks:
- A non-transactional read/write to a TM-tracked address is flagged as a violation (unless bypass is active).
- TM reads inside non-transactional code are flagged.

### 4.2 Opacity / Read-set Validation

The TM model validates the read set at commit time. If an orec version changed since the transaction's start, the simulator logs an opacity violation (which would cause an abort in real TM but might be silent in buggy implementations).

### 4.3 Deadlock / Livelock Detection

- **Deadlock**: A cycle in the wait-for graph (thread A holds lock L1, waits for L2; thread B holds L2, waits for L1). Detected by building the graph on each lock acquire attempt.
- **Livelock**: A transaction that retries > N times (N configurable) is reported.

### 4.4 Double-Free / Bad Pointer (Opacity Violations)

The simulator maintains a shadow memory for the TM region:
- Each allocation is tracked (start address + size).
- On `free`, the address is checked: already freed → double-free; not allocated → bad pointer.
- The deferred-free list is modeled separately: pointers are moved from live to deferred to retired; freed only when safe (EBR).

### 4.5 TM Address Space Violations

The simulator knows which addresses are in the TM region (mmap range from tm_region_allocator) and which are heap/stack. A TM read/write to a non-TM address is flagged during simulation (mirrors `LLVM_TM_ADDR_CHECK` in release mode: the write bypasses TM logic and goes directly to memory — but the simulator records the violation).

---

## 5. Integration with the Fuzz Tool

The fuzz tool (in `tests/`) generates random TM workloads as C++ files. The flow:

```
fuzz_tool → .cpp → compile with plugin(--emit-tm-trace) → trace.jsonl → simulator
```

The simulator replays the trace and checks correctness. If a violation is found:
1. The simulator exits with a non-zero code and the offending event.
2. The fuzz harness can bisect to the minimal failing input.
3. The checkpoint allows restarting just before the violation for detailed debugging.

For rapid testing, the fuzz tool can also feed events directly to the simulator via stdin (pipe mode), skipping file I/O.

---

## 6. Project Structure

```
simulator/
├── Cargo.toml
├── PLAN.md                     ← this file
├── src/
│   ├── main.rs                 ← CLI entry point
│   ├── engine.rs               ← DES event loop, checkpoint/restore
│   ├── event.rs                ← Event, EventKind, ordering
│   ├── queue.rs                ← BinaryHeap wrapper + checkpoint
│   ├── lp.rs                   ← Logical process (thread state, TM model)
│   ├── tm_model.rs             ← TM model (begin, read, write, commit, abort)
│   ├── memory.rs               ← Shadow memory, TM address space tracker
│   ├── checker.rs              ← Verification rules (opacity, deadlock, etc.)
│   ├── rng.rs                  ← Checkpointable PRNG
│   └── trace.rs                ← JSONL trace parser
```

### CLI

```
tm-des [--trace trace.jsonl] [--checkpoint checkpoint.bin]
       [--checkpoint-every N] [--seed S] [--max-events M]
       [--livelock-threshold T] [--deadlock-timeout D]
```

- `--trace`: input trace file (or `-` for stdin).
- `--checkpoint`: restore from checkpoint instead of starting fresh.
- `--checkpoint-every N`: save checkpoint every N events.
- `--seed`: global PRNG seed.
- `--max-events`: stop after processing M events.
- `--livelock-threshold`: retries before livelock warning (default 1000).
- `--deadlock-timeout`: spin iterations before deadlock suspicion (default 10000).

---

## 7. Implementation Phases

| Phase | Scope | Deliverable |
|-------|-------|-------------|
| P1    | Rust scaffold: event, queue, engine, CLI | `cargo run -- --trace test.jsonl` processes events |
| P2    | Checkpointable PRNG + checkpoint/restore | Can save/restore simulation state |
| P3    | TM model (begin/read/write/commit/abort) | Simulates STM transactions |
| P4    | Shadow memory + alloc tracking + deferred-free | Detects double-free, bad pointers |
| P5    | Verifiers: opacity, deadlock, livelock, address-space | Reports violations |
| P6    | LLVM plugin —emit-tm-trace flag | Trace file from real benchmarks |
| P7    | Fuzz-tool integration: pipe mode, bisect | End-to-end: fuzz → trace → simulate → report |
| P8    | Stress-test all backends through simulator | Compare real vs simulated TM behavior |

---

## 8. Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| TM model diverges from real implementation | Model is parameterized (orec count, clock policy, retry strategy). Backend-specific configs read from TOML. |
| Trace emission slows plugin too much | Trace emission is gated by `--emit-tm-trace` flag; off by default. Batched writes. |
| Checkpoint size explodes | Only snapshot LP state + event tail; memory contents are hashed (Merkle) for equality checks. |
| DES too slow for large traces | Use `--max-events` for focused debugging; full traces run in batch mode. |
