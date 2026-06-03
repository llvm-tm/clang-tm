# Implementation Notes: Romulus

## Overview

**Romulus** (RObust Multi-version Lock-Free Solution) is a practical implementation of Left-Right Synchronization designed for shared-memory multiprocessor systems. It addresses the challenge of coordinating concurrent transactions without traditional locking.

## Key Design Principles

### 1. Lock-Free Coordination

Romulus uses **atomic operations** (CAS - Compare-And-Swap) instead of traditional locks to avoid:
- Lock contention
- Deadlock
- Priority inversion

### 2. Multi-Versioning

Multiple versions of shared data structures allow concurrent reads without blocking writes.

### 3. Bipartite Execution

Each transaction goes through two phases:
- **Phase L (Left)**: Read and validate
- **Phase R (Right)**: Write and commit

### 4. Timestamp-Based Ordering

Each transaction receives a unique timestamp to establish a total ordering.

## Architecture

### Transaction Structure

```c
struct Transaction {
    uint64_t timestamp;      // Global ordering
    uint64_t left_id;        // Left phase ID
    uint64_t right_id;       // Right phase ID
    bool committed;
    // ... other fields
};
```

### Shared Memory Layout

```
┌─────────────────────────────────────────────────────┐
│                    Shared Memory                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
│  │Version 1    │  │Version 2    │  │Version 3    │ │
│  │  (Read Only)│  │  (Read Only)│  │  (Read Only)│ │
│  └─────────────┘  └─────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────────┘
             ▲                ▲
             │                │
       ┌─────┴─────┐    ┌─────┴─────┐
       │  Left     │    │  Right    │
       │  Phase    │    │  Phase    │
       └───────────┘    └───────────┘
```

## Synchronization Mechanism

### Coordination Buffer

```
┌─────────────────────────────────────────────────────┐
│              Coordination Buffer                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
│  │  Left Queue │  │  Right Q.   │  │  Commit Q.  │ │
│  └─────────────┘  └─────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────────┘
```

### Key Operations

1. **Transaction Creation**: Allocate and initialize a new transaction with a unique timestamp
2. **Left Phase Execution**: Read data, validate dependencies, prepare for commit
3. **Right Phase Execution**: Write changes, validate write-set, commit

## Advantages

1. **High Scalability**: No lock contention
2. **No Deadlocks**: Lock-free by design
3. **Strong Consistency**: Total ordering guarantees
4. **Multi-Version Support**: Allows concurrent reads and writes

## Trade-offs

- **Memory Overhead**: Multiple versions of data
- **Complexity**: More sophisticated than simple locking
- **Latency**: May have higher latency than pessimistic approaches for write-heavy workloads

## Use Cases

- High-contention transactional memory systems
- Large-scale parallel computing
- Systems requiring strong consistency without locks

---

*Note: This explanation is based on the general principles of Romulus as described in the referenced paper.*