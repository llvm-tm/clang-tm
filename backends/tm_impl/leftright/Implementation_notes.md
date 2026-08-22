# Implementation Notes: Left-Right Synchronization

> **⚠️ Name-vs-content caution:** The `leftright` backend in this repository
> is **not** an implementation of the Left-Right synchronization pattern
> described below. It is a **global-clock OCC** design (snapshot timestamp,
> read-set validation, commit-lock write-back — structurally similar to the
> `romulus` backend). The generic Left-Right theory in this file is kept as
> background reading only; for what the backend actually does, read
> `leftright.hpp` (value-based validation under a commit lock) and the
> history in `AGENTS.md` (2026-06-20 session).

## Overview

Left-Right Synchronization (LRS) is a general framework for coordinating concurrent processes that is based on the principle of **Left-Right ordering**. The system ensures that all "Left" operations from one process are ordered before any "Right" operations from another process.

## Core Concepts

### Left-Right Ordering Principle

The fundamental idea is to split each operation into two phases:
- **Left Phase**: Operations that must happen **before** a synchronization point
- **Right Phase**: Operations that must happen **after** a synchronization point

The key invariant is: **All Left operations from one process must be ordered before any Right operations from another process.**

### Transaction Model

Each transaction is treated as a sequence of operations with a clear Left-Right boundary:

```
Transaction: [Left Operations] | [Right Operations]
```

### Synchronization Mechanism

The coordination is achieved through a shared coordination mechanism that:
1. Enforces global ordering of transactions
2. Ensures Left-Right ordering between different transactions
3. Detects and handles conflicts

## Implementation Architecture

### Key Components

1. **Left-Right Queue**: A queue that separates Left and Right operations
2. **Timestamp System**: Assigns unique timestamps to establish ordering
3. **Conflict Detector**: Identifies when transactions conflict
4. **Commit Manager**: Handles transaction completion

### Synchronization Protocol

```
┌─────────────────────────────────────────────────────┐
│              Thread 1                               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
│  │  Left Op    │  │   Barrier   │  │ Right Op    │ │
│  └─────────────┘  └─────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│              Thread 2                               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
│  │  Left Op    │  │   Barrier   │  │ Right Op    │ │
│  └─────────────┘  └─────────────┘  └─────────────┘ │
└─────────────────────────────────────────────────────┘

      ↓                              ↓

  ┌─────────────────────────────────────────────────────┐
  │              Coordination Mechanism                 │
  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
  │  │  Left Q.    │  │  Right Q.   │  │  Commit Q.  │ │
  │  └─────────────┘  └─────────────┘  └─────────────┘ │
  └─────────────────────────────────────────────────────┘
```

## Advantages

1. **Scalability**: Reduces contention compared to traditional locks
2. **Flexibility**: Can be applied to various synchronization scenarios
3. **Composability**: Multiple synchronization points can be chained

## Trade-offs

- **Complexity**: More sophisticated than simple locking
- **Overhead**: Additional memory and processing for coordination
- **Latency**: May have higher latency than pessimistic approaches for some workloads

## Use Cases

- Transactional memory systems
- Database concurrency control
- Distributed systems coordination
- Parallel computing synchronization

---

*Note: This explanation is based on the general principles of Left-Right Synchronization as described in the referenced paper.*