# Audit: TSXSGL (TSX + Single Global Lock)

**Score: 4/5** — Core dual-mode protocol well-captured; TSX hardware details (capacity, abort reasons) abstracted; fence annotations (`lastFence`+`FenceFidelity`) added.

## Files

| Artifact | Path |
|----------|------|
| TLA+ spec | `docs/proofs/tsxsgl.tla` (PlusCal, 343 lines) |
| C++ runtime | `backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp` (294 lines) |
| TLC config | `docs/proofs/tsxsgl.cfg` |

## Algorithm Summary

Hybrid TSX+SGL: try TSX transaction first (5 attempts), if unavailable or retries exhausted, fall back to SGL. TSX abort detection: re-read `sgl_owner` at commit; if changed (concurrent SGL wrote to it), abort. SGL writes 1 to `sgl_owner` so concurrent TSX transactions abort on cache-coherence.

## Cross-Reference Checklist

| C++ | TLA+ | Match | Notes |
|-----|------|-------|-------|
| begin(): _xbegin(), check sgl_owner==0 | L_idle: Start TSX: await sgl=0, mode=tsx | ✅ | |
| begin(): sgl_owner.store(1), global_tx_lock.lock() | L_idle: Start SGL: await sgl=0, sgl:=self | ⚠️ Partial | C++ stores 1, not thread-id |
| end(): in_tsx → re-read sgl_owner → _xend | L_active: Commit TSX: check sgl unchanged | ✅ | |
| end(): sgl_owner.store(0), unlock | L_active: Commit SGL: sgl:=0 | ✅ | |
| Read/Write: direct memory access | L_active: Read/Write: mem direct | ✅ | |
| _xabort(LOCK_BUSY) on contention | L_active: TSX abort (sgl changed) | ✅ | |
| SGL fallback after max retries | L_active: TSX fallback to SGL | ✅ | |

## Invariants

| Invariant | C++ check | TLC result |
|-----------|-----------|------------|
| LockFreeInv: sgl=0 ⇔ no thread in SGL | sgl_owner tracks SGL state | ✅ PASS |
| LockOwnerInv: sgl=t ⇒ mode[t]=sgl | Owner tracking | ✅ PASS |
| AtMostOneSGL | std::mutex guarantees | ✅ PASS |

## Deviations

### 1. TSX read-set = entire address space (Medium risk)
**TLA+** (`tsxsgl.tla:55`): `readSet[self] := Addr` — the TSX hardware captures the entire address space.
**C++**: Intel TSX captures accessed cache lines in the L1 cache, with capacity limits (~128 lines, ~8KB L1 data). Exceeding capacity causes a capacity abort.

The model assumes TSX always succeeds (within retry limit) for any workload. In reality, large transactions exceed capacity and fall back to SGL.

**Risk**: Medium — the model may over-approximate TSX success, missing paths where capacity aborts force SGL fallback.

### 2. sgl_owner stores thread-id vs sentinel 1 (No risk)
**TLA+** (`tsxsgl.tla:61`): `sgl := self` (thread-id).
**C++** (`TSXSGL_runtime.cpp:209`): `sgl_owner.store(1, ...)` (sentinel).

The C++ code uses 1 because the hardware only needs to detect that a write to the cache line occurred — any non-zero value will cause a TSX abort. The TLA+ model uses the thread-id for stronger invariants (LockOwnerInv, LockFreeInv).

**Risk**: None — the property being checked (mutual exclusion, SGL-active detection) is the same.

### 3. No capacity abort model (Medium risk)
**C++**: Intel TSX capacity aborts occur when the read-set exceeds L1 cache capacity (~128 cache lines). The C++ runtime has no control over this — the hardware decides.
**TLA+**: No capacity tracking. TSX aborts only when `sgl != txSnapshot[self]`.

**Risk**: Medium — capacity aborts are a real correctness concern. If a TSX transaction aborts due to capacity, it falls back to SGL. The model doesn't capture this path.

### 4. Abort reason handling (Low risk)
**C++** (`TSXSGL_runtime.cpp:196–201`): Checks `_XABORT_EXPLICIT` and `_XABORT_CODE(status) == LOCK_BUSY` to decide whether to spin-wait vs break. Also checks `_XABORT_RETRY` to decide whether to retry.
**TLA+**: Simple boolean abort with `tsxRetries` counter. No abort reason differentiation.

**Risk**: Low — abort reason handling is a performance optimization (avoid spurious spinning). Correctness is unaffected.

### 5. 5-attempt retry vs MaxRetries configurable (No risk)
**C++**: Hardcoded 5 attempts.
**TLA+**: `MaxRetries` constant (configurable in .cfg).

**Risk**: None — parameterization doesn't affect safety.

## Summary

| Aspect | Verdict |
|--------|---------|
| Dual-mode protocol (TSX/SGL) | ✅ Good match |
| Invariants | ✅ All pass TLC |
| TSX capacity model | ❌ Not modeled |
| Abort reason handling | ❌ Not modeled |
| **Overall score** | **3/5** |
