# STMbench7 Implementation Notes

## Standard Reference
Guerraoui, Kapalka, Vitek. "STMBench7: A Benchmark for Software Transactional Memory." EuroSys 2007.

## Duration Parameter

The standard runs for a **fixed wall-clock duration** (not a fixed number of operations).

| Flag | Default | Description |
|------|---------|-------------|
| `-d` | 10000 ms | Duration in milliseconds (`-d 10000` = 10 seconds) |

Within that window, threads execute a random mix of the 45 operations as fast as they can, constantly selecting new operations. The number of completed operations per second (throughput) is the reported metric.

Operations read the clock at startup and pre-compute the total number of iterations (operations × 10 / thread). Each thread finishes when it has done its share, or when the wall-clock sleep expires, whichever comes first.

```
int loops = duration_ms / 10;          // per-thread operation count
// ... run `loops` operations ...
std::this_thread::sleep_for(duration_ms);  // full-duration sleep
```

This is the standard methodology: let all threads run for `duration_ms`, measure how many operations completed.

## Workload Types (from spec)

| Workload | Read % | Update % |
|----------|--------|----------|
| 1        | 90%    | 10%      |
| 2        | 60%    | 40%      |
| 3        | 10%    | 90%      |

## Category Distribution (from spec)

| Category               | Symbol | Target % | Verification |
|------------------------|--------|----------|--------------|
| Long traversals        | LT     | ~5%      | 4.5–5.5%     |
| Short traversals       | ST     | ~40%     | 38–42%       |
| Short operations       | OP     | ~45%     | 43–47%       |
| Structure modifications| SM     | ~10%     | 9–11%        |

## Data Structure (Medium OO7)

| Element               | Count (medium OO7) |
|-----------------------|--------------------|
| Modules               | 1                  |
| ComplexAssemblies     | 364                |
| BaseAssemblies        | 729                |
| CompositeParts        | 500                |
| AtomicParts           | 100000             |
| Connections           | 300000             |
| Documents             | 500                |

## Verified Configurations (TinySTM WBCTL)

| Threads | Duration | Workload | Ops/sec | Category match |
|---------|----------|----------|---------|----------------|
| 4       | 10s      | 1        | ~400    | Yes (LT 4.7%, ST 40.8%, OP 44.4%, SM 10.1%) |
| 4       | 10s      | 2        | ~200    | Yes |
| 4       | 10s      | 3        | ~100    | Yes |

## Known Issues

- **NOrec**: PASS (1000 ops in 10s at 1t, category distribution matches spec)
- **TL2**: FAIL — SIGSEGV before any TX completes. Likely null-pointer deref from STL container internal use-with-clone mismatch.
- **TinySTM WT**: FAIL — HANG (encounter-time + write-through creates TX-level livelock on long traversals). No output past data-structure init.
- **SwissTM**: FAIL — HANG (same mechanism as WT).
