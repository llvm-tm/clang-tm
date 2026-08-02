# gpu_ycsb.cu — SIMD Execution Model Review

Date: 2026-07-31

## Summary

`gpu_ycsb.cu` follows the **warp-cooperative** execution model (one warp per
transaction), matching Epic's model rather than the thread-per-transaction
model of GPUTx/Caracal. Control flow is warp-uniform: all 32 lanes of a warp
execute the same instruction stream on the same version list, so branch
divergence is essentially zero by construction.

## SIMD conformance checklist

| Requirement | Status | Where |
|-------------|--------|-------|
| All threads do the same code | ✅ | `tx_ycsb` body runs identically on all 32 lanes |
| Threads operate on different data | ✅ | each warp gets its own `warp_id` → own RNG → own records |
| No data-dependent `if`/loops causing divergence | ✅ | loop bound `a->ops` and write-ratio branch are warp-uniform (`r` and `a->write_ratio` identical on all lanes) |
| Warp-cooperative memory access | ✅ | `csmv_gpu_read` walks the version list with all lanes in lockstep via `__ballot_sync`/`__shfl_sync` |
| Leader-only mutations | ✅ | read-set/write-set recorded only by lane 0; commit write-back only lane 0 |

## Key design decisions

1. **Warp-per-tx, not thread-per-tx.** Each warp cooperatively executes one
   transaction. The version-list traversal uses all 32 lanes in parallel
   (each lane checks a node, `__ballot_sync` finds the first match,
   `__shfl_sync` broadcasts the value). This avoids per-lane divergent TM
   calls and matches Epic's "warp-cooperative execution avoids branch
   divergence" rationale.

2. **Uniform RNG.** The xorshift sequence is derived from `warp_id` only, so
   every lane in a warp derives the same record index and the same
   read/write decision. There is no lane-dependent data → no divergence.

3. **Read-only / write-mixed single body.** The write-ratio branch is uniform
   across the warp, so the predicate evaluation is cheap (one uniform branch,
   no per-lane mask).

## Remaining divergence risk

The only non-uniform control flow is inside the backend primitives
(`if (lane_id == 0)` leader patterns), which are deliberate and handled with
convergence intrinsics. **Caution:** these rely on `__activemask()`. If a
future tx body introduces lane-divergent early returns *before* calling
`csmv_gpu_read`/`csmv_gpu_commit`, the ballot/shuffle masks would be wrong.
Rule: **all 32 lanes must call every `csmv_gpu_*` primitive**.

## Recommended improvements (future)

- **AoS → SoA**: the `CSMVWarpState` write-set/read-set arrays are per-warp;
  for higher occupancy consider splitting read/write set storage so lane 0
  only ever touches them (already done) and using shared-memory staging.
- **Epoch batching**: like Epic, execute multiple independent txns per warp
  to amortize version-list traversal.
- **`__activemask()` → explicit mask**: pass `0xffffffff` (or `__full_mask()`)
  to `__ballot_sync`/`__shfl_sync` when the full warp is guaranteed active,
  which the current kernel guarantees (guard `warp_id >= num_txns` returns
  uniformly).
