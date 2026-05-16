# Research Notes: Alternative Backend Designs

This file assesses the feasibility of implementing two more
sophisticated backends: one for **persistent TM** (DUDE-TM / SPHT)
and one for **distributed TM** (sockets + remote servers).

---

## 1. Persistent STM — DUDE-TM / SPHT

### Background

- **DUDE-TM** (Durable User-Defined STM Extensions) is a framework
  for building durable (persistent) transactional memory on NVM.
  It decouples the STM algorithm from the persistence mechanism.
- **SPHT** (Single-Phase Hybrid TM) combines hardware and software TM
  with persistence support, targeting Intel Optane DC PM.

### Feasibility

The LLVM plugin already provides:

- `tm_begin`/`tm_end` — transaction boundaries
- `tm_read_*`/`tm_write_*` — instrumented load/store hooks
- `tm_malloc`/`tm_free` — transactional allocation

To implement DUDE-TM or SPHT, a runtime would need:

| Requirement                   | Status                                             |
|-------------------------------|----------------------------------------------------|
| Persistent memory allocation  | Already done (PersistentSGL bump allocator)        |
| Write-ahead logging (WAL)     | Not implemented — would need undo/redo log in NVM  |
| Cache-line flush + fence      | Requires `clwb`/`clflushopt` + `sfence` (x86) or   |
|                               | `dc cvap` + `dsb sy` (ARM).  Currently not used.   |
| Failure-atomic sections       | Plugin wraps TM globals as plain variables; no      |
|                               | persistent memory annotation (e.g. `pmem` attr).    |
| NVM-aware allocator           | Could extend PersistentSGL's bump allocator with a  |
|                               | free-list and crash-recovery marking.               |

### Recommended approach

**Separate runtime file** (`DurableTM_runtime.cpp`) that:

1. Uses PersistentSGL's mmap and bump allocator as a starting point.
2. Adds a redo-log in the persistent region: on `tm_write`, append
   `(addr, value)` to the log.
3. On `tm_end`: flush the log with `clwb` + `sfence`, atomically
   commit by updating a persistent epoch counter, then replay the
   log to main memory.
4. On restart: scan the log, replay incomplete commits, discard
   aborted transactions.

Estimated effort: **moderate** (a few days).  The plugin hook
interface is already sufficient; the missing pieces are all in the
runtime (cache-line flushes, log format, recovery scan).

---

## 2. Distributed TM — Sockets + Remote Servers

### Background

A true distributed TM would replace the shared-memory single lock
with a network protocol.  Each server holds a partition of the data;
transactions that touch multiple partitions use a distributed
commit protocol (2PC, Paxos, or Raft).

### Feasibility

The plugin gives us `tm_begin`/`tm_end`/`tm_read_*`/`tm_write_*`.
For a socket-based distributed TM, a runtime would need:

| Requirement                   | Status                                           |
|-------------------------------|--------------------------------------------------|
| Network communication         | Not implemented — would need sockets (or gRPC).   |
| Partition mapping             | Address→server hash function; not implemented.    |
| Distributed 2PC coordinator   | Not implemented.  DistributedSGL uses shared mmap |
|                               | instead of sockets.                               |
| Distributed deadlock detection| Not implemented.                                  |
| Read-set / write-set logging  | Not done in DistributedSGL (uses full-state sync).|
| Serialisation format          | Would need to pack instrumented variables into    |
|                               | network messages (protobuf, FlatBuffers, etc.).   |
| Concurrency control           | DistributedSGL uses a single global lock (no      |
|                               | concurrent commits).                               |

### Recommended approach

**Separate runtime** that builds on DistributedSGL but replaces the
shared mmap with real sockets:

1. **Start** with DistributedSGL's 2PC structure (PREPARE → DATA →
   COMMIT phases) already modelled in `tm_begin`/`tm_end`.
2. **Replace** `msync` + `std::atomic` on mmap with TCP messages:
   - `tm_begin`: send PREPARE to all replicas, wait for ACKs.
   - `tm_end`: send COMMIT/ABORT to all replicas.
   - On ABORT: replay from coordinator if needed.
3. **Add** a partition map: static hash of TM-global address → server
   ID.  Loads/stores to addresses on remote partitions become RPCs.
4. **Add** read-set logging inside the runtime: record every
   `tm_read_*` address/value locally, send only the write-set during
   2PC.

Estimated effort: **high** (weeks).  The plugin hook interface is
sufficient, but the runtime is essentially building a distributed
OLTP system.  A simpler first step would be to replace the mmap
with Unix domain sockets on the same machine (no network stack),
then extend to TCP.

---

## Summary

| Approach               | Feasibility | Est. effort | Plugin changes needed |
|------------------------|-------------|-------------|-----------------------|
| DUDE-TM / SPHT runtime | High        | Days        | None (just runtime)   |
| Sockets distributed TM | Medium      | Weeks       | None (just runtime)   |

Both approaches are **fully supported** by the plugin's current hook
interface.  No LLVM pass changes are required — only a new runtime
`.cpp` file and (for distributed) serialisation helpers.

The main engineering challenges are:

- **Persistent**: crash-safe logging, pmem cache-line flushing,
  allocator recovery.
- **Distributed**: network I/O, partition mapping, serialisation,
  distributed consensus.
