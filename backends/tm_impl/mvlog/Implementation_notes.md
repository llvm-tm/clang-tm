# MVLog — Multi-Version Log-based STM (Design)

**Status: implemented in C++ (`MVLog.hpp` / `MVLog_globals.hpp` / `MVLog_runtime.cpp`)
and Rust (`expli_instr/rust/workspace/runtime/mvlog/`). Verified: `test_tx` 114/114,
`test_ds` 207/207, fuzz_counter/fuzz_bank multi-thread PASS; TLA+ model safety PASS.
Design document retained below for the protocol, correctness lemma, and open questions.**

MVLog is a shared-memory STM whose commit record is a *single, evergrowing,
append-only log of write-sets*. Every transaction negotiates its commit
position (a log slot) *before it starts executing*. The log lets late readers
"snoop" earlier committed writes directly instead of overwriting shared
memory in place, and it lets the commit protocol serialize purely by log
position.

The design satisfies six requirements:

1. **Log write sets like SPHT.** A committed transaction appends `(addr, val)`
   pairs to a global commit log (SPHT logs to a per-thread PCL; MVLog appends
   to one shared, ordered log).
2. **Per-address index.** A map `addr -> slot` points to the newest committed
   log entry that wrote `addr`, so a reader finds the new value of an address
   in O(1) instead of scanning the log.
3. **Slot negotiated at begin.** `tm_begin()` performs one atomic
   `fetch_add` on a global `next` counter; the returned value is the
   transaction's commit slot.
4. **Fast dirty filter.** A Bloom filter of *written addresses* lets a reader
   skip the index entirely in the common case (read plain memory). The filter
   has **no false negatives** — a "clean" answer is exact; a "dirty" answer
   falls back to the index/log.
5. **Snoop committed predecessors.** Because a transaction knows its slot `S`
   from the start, and *no transaction with slot `> S` can commit while it is
   in flight* (they must wait on `S`), any committed entry it can see through
   the index is a committed predecessor (`< S`) — the value is the right one
   for its snapshot.
6. **Wait, then abort on conflict.** At commit a transaction waits until every
   predecessor slot resolves, then value-validates its read-set; any
   conflicting predecessor write (different value) causes an abort.

---

## 1. Why a log + index instead of in-place commit

Most OCC backends (TL2, NOrec-BF, Romulus) write values **in place** and use a
clock/version to validate. MVLog instead publishes writes to an append-only
log for two reasons:

- **Readers never block and never need a lock.** A read resolves through the
  index (one pointer chase) or plain memory (Bloom-filter miss). There is no
  lock bit, no version table, no read-validate fence on the read path.
- **Commit is a pure append.** The write-set is stored once; readers pointing
  at the log entry see immutable data. No write-back race, no torn snapshot
  during commit (a reader can never observe a half-published write-set because
  the entry is made visible only after it is fully written, and predecessors
  are guaranteed resolved).

The log is also naturally persistent-friendly (the notes' "future work"
section sketches NVM adaptation).

## 2. Data structures

Global:

| Structure | Role |
|-----------|------|
| `g_next` (`std::atomic<uint64_t>`) | Allocates commit slots; `fetch_add(1)` at `tm_begin`. |
| `g_log[]` | Append-only commit log. Entry `s` is `(state, write_set, read_set)`. State ∈ {`FREE`, `PROGRESS`, `COMMITTED`, `ABORTED`}. |
| `g_index` | Per-address map `addr -> slot` of the newest **committed** writer (`-1` = never written). |
| `g_dirty` | Bloom filter of addresses written by committed-but-unreclaimed log entries (no false negatives). |
| `g_mem[]` | TM memory region. Holds the **retired** (reclaimed) value; authoritative only for addresses absent from `g_dirty`. |

Per transaction (thread-local):

| Structure | Role |
|-----------|------|
| `slot` | The claimed commit slot (fixed at begin). |
| `read_set` | `(addr, value)` pairs observed by the transaction. |
| `write_set` | `(addr, value)` buffered writes (read-own-writes checked first). |

## 3. Protocol

### 3.1 Begin

```cpp
tx->slot = g_next.fetch_add(1, memory_order_acq_rel);   // negotiate commit position
tx->read_set.clear();
tx->write_set.clear();
g_log[tx->slot].state = PROGRESS;                        // publish "in flight"
```

The atomic fetch-add is the only global RMW on the common path (compare:
TL2's read path needs a `fetch_add`-free clock read but writers CAS per guard;
NOrec reads a global clock; SGL takes a mutex).

### 3.2 Read `tm_read(addr)`

```cpp
if (write_set contains addr) return write_set[addr];     // read-own-writes
if (!g_dirty.may_contain(addr)) {                        // FAST PATH (Bloom miss)
    v = g_mem[addr];                                     // exact: no live writer
} else {                                                 // SLOW PATH (snoop)
    slot_t w = g_index[addr];                            // newest committed writer
    v = (w == -1) ? g_mem[addr] : g_log[w].write_set[addr];
}
read_set.push_back({addr, v});
return v;
```

**Correctness of the fast path.** `g_dirty` has no false negatives: if a log
entry wrote `addr` and its write has not yet been reclaimed into `g_mem`, then
`addr` is present in the filter. A miss therefore means `g_mem[addr]` holds
the newest committed value.

**Why the snoop is always the *right* version.** Only committed entries are
reachable through `g_index` (it is updated at commit). While a transaction at
slot `S` is in flight, no transaction with slot `> S` can commit — a committer
at slot `U` waits until *all* slots `< U` resolve, including `S`. Hence every
entry the reader can see via the index has slot `< S`, which is exactly the
visible prefix. (`g_index` never points at an in-progress entry, so reads
never wait on an uncommitted writer.)

### 3.3 Write `tm_write(addr, val)`

```cpp
write_set[addr] = val;                                    // buffer only, no memory access
```

(No log append during the body — the write-set is appended atomically at
commit, which is what makes commit a pure publish.)

### 3.4 Commit

```cpp
// Phase 1 — wait for predecessors (requirement 6)
for (s = 0; s < tx->slot; s++)
    while (g_log[s].state == PROGRESS) cpu_relax();       // acyclic: s < slot

// Phase 2 — value-validate the read-set (requirement 6)
for (auto &[addr, captured] : read_set) {
    w = g_index[addr];                                    // all predecessors resolved
    cur = (w == -1) ? g_mem[addr] : g_log[w].write_set[addr];
    if (cur != captured) goto ABORT;
}

// Phase 3 — publish
g_log[tx->slot].state = COMMITTED;
g_log[tx->slot].write_set = write_set;
g_log[tx->slot].read_set  = read_set;
for (auto &[addr, val] : write_set) g_index[addr] = tx->slot;   // update index
g_dirty.insert_all(write_set.keys());                           // mark dirty
commit();                                                       // increment counter, reset
```

Read-only transactions take the identical path except their write-set is
empty: they still wait for predecessors and validate, then mark the slot
`COMMITTED` with an empty write-set.

**Why read-only transactions must validate.** A naive "read-only never
aborts" design reads *monotonically* (each read sees the newest committed
predecessor at read time) rather than a single snapshot. A predecessor `P`
that commits *between* two of the reader's reads can shift one address to a
newer version while another address stays old — the classic **read-skew**
(`read a=1`, `P: a=2,b=2` commits, `read b=2` → `(a=1,b=2)` never existed).
Validating the read-set against the (now final) index at commit makes every
read-only transaction serializable. The TLA+ model's
`CommittedReadsConsistent` invariant is precisely this.

**Why value validation instead of slot/version capture.** The fast path reads
plain memory and cannot cheaply capture a version. Re-resolving each read
address at commit (all predecessors final) and comparing the value is sound:
a value change implies a conflicting predecessor write; an unchanged value
implies a valid read (even if a predecessor rewrote the *same* value).

### 3.5 Abort

```cpp
g_log[tx->slot].state = ABORTED;
abort_tx();   // longjmp to retry loop
```

An aborted slot still "resolves", so successors waiting on it proceed. Aborted
writes never enter `g_index` or `g_dirty` and are never visible.

## 4. Ordering, liveness, and the no-`>S`-commits lemma

**Lemma.** While transaction `S` is in flight, no transaction with slot
`U > S` can commit.

*Proof.* A committer at slot `U` waits for every slot `s < U` to resolve
(Phase 1). `S < U`, so the committer waits on `S`. `S` resolves only when `S`
commits or aborts. ∎

**Consequences.**
- Reads are non-blocking and always see the committed prefix `< S`.
- Commit order equals slot-claim order (a total order), so wait-for-all-*predecessors*
  + value-validation yields **opacity / serializability** (no read skew, no
  write skew, no lost update).
- The wait graph is acyclic (each wait goes to strictly smaller slots), so no
  deadlock.

**Costs.**
- **Head-of-line blocking.** A long-running transaction at a low slot blocks
  all subsequent *commits* (not reads) until it resolves. This is the price of
  a strictly ordered commit log; it is acceptable for workloads with short
  transactions.
- One atomic `fetch_add` per transaction (the slot claim).
- The Bloom filter may push some reads to the snoop path (false positives);
  each miss is exact.

## 5. Reclamation (log prefix → memory)

The log grows forever; retired prefixes must eventually be folded back into
`g_mem`:

1. Reclaim a prefix only at a **quiescence point** (e.g. a thread-exit or a
   barrier) where no in-flight transaction has a slot inside the prefix. This
   is required so that a reader can never see a *too-new* `g_mem` value.
2. For each entry in the prefix, write its `write_set` into `g_mem` (in slot
   order), then advance the reclamation watermark.
3. Rotate `g_dirty`: when the filter's popcount crosses a threshold, start a
   fresh filter for the new generation and retire the old one; readers check
   all active generations. Bloom filters cannot be cleared element-wise, so
   generation rotation is the standard scheme.

**Fast-path invariant after reclamation.** If `addr ∉ g_dirty` then
`g_mem[addr] = ReadValue(addr, S)` for every in-flight reader `S`, because the
prefix was reclaimed only when no in-flight reader's slot was inside it.

The core protocol in the TLA+ model abstracts reclamation (see §7); the model
uses a no-reclamation `dirty` set, for which the fast path is correct by the
same argument (a miss implies no committed writer).

## 6. Relationship to prior work

- **SPHT** — write-set logging; MVLog replaces the per-thread PCL + RTM with a
  shared ordered log + index + Bloom filter (software conflict detection).
- **RingSTM / LSA** — ordered commit logs with Bloom-filter read-sets; MVLog's
  slot-at-begin is RingSTM's ring-slot negotiation, with a direct per-address
  index and a value-validated commit (no re-scan of the ring at commit).
- **NOrec-BF / TL2** — MVLog's validation is value-based (like NOrec-BF's
  global filter + sequence-lock double-check) but the commit order is a log
  position, not a clock, and readers never contend on a global clock.

## 7. TLA+ model

`docs/proofs/MVLog.tla` models the core protocol:

- Slots, `index`, `dirty`, `mem`, read/write sets, and the commit wait +
  value validation.
- **`CommittedReadsConsistent`** — for every committed slot `s`, every
  recorded read `<<addr, v>>` satisfies `v = ReadValue(addr, s)` (opacity).
  This is the property that catches the read-skew bug of the naive
  read-only-never-aborts design.
- **`IndexIsHighest`** — `index[addr]` is the newest committed writer.
- **`DirtySuperset`** — no false negatives (every committed writer's
  addresses are in `dirty`), which is what makes the Bloom-miss fast path
  exact.
- Plus slot-state and fence-fidelity invariants, in the same style as
  `JVSTM.tla` / `Calvin.tla`.

## 8. Open questions / future work

- **NVM / persistence**: the append-only log is a natural fit for persistent
  memory (append entries + flush watermark), like SPHT's group commit.
- **Snapshot read-only transactions**: if a workload wants never-aborting
  read-only transactions, an alternative is to take a *read version* (a
  quiescent watermark) at begin and validate that no newer committed writer
  intersects the read-set — at the cost of a reclamation-aware index. The
  current uniform wait+validate design is simpler and is what is modeled.
- **Per-slot groups / epochs** to amortize the per-slot spin, and to let a
  batch of transactions commit together (reducing head-of-line blocking).
- **Removing head-of-line blocking** via a skip-list-style "resolved
  watermark" so committers wait on the watermark instead of scanning slots.
- **Full reclamation model** in TLA+ (quiescence + generation rotation) is
  future work; the current model verifies the log/index protocol with an exact
  dirty set.
