# Jmp_buf Meta-Invariant Plan

## Problem

`siglongjmp` to a `jmp_buf` whose stack frame has already returned is undefined
behavior. The TLA+ models abstract the retry mechanism entirely (reset `readSet`
`writeSet`, goto `L_idle`), so they cannot catch this class of UB.

## Design

Add a meta-model layer wrapping any backend TLA+ model. This layer tracks frame
lifetimes and `jmp_buf` validity as a cross-cutting invariant.

### State variables

- **`FrameStack[t]`**: sequence of frame IDs for thread `t` (stack grows on
  `sigsetjmp`, shrinks on `return`).
- **`ValidJmpBuf[t]`**: the frame ID whose `jmp_buf` is currently valid for
  thread `t`. Set by `sigsetjmp`, cleared when that frame returns.
- **`InTx[t]`**: boolean — `TRUE` between `sigsetjmp(mp)` and the matching
  `siglongjmp_or_commit`.

### Transitions (interleaved with backend actions)

| Action | Effect |
|--------|--------|
| `sigsetjmp(mp, flags)` | Push frame ID to `FrameStack[t]`. Set `ValidJmpBuf[t] := frame_id`. Set `InTx[t] := TRUE`. |
| `siglongjmp(mp, val)` | Assert `ValidJmpBuf[t] = mp.frame_id` (invariant check). Reset state. |
| Frame return | Pop `FrameStack[t]`. If `ValidJmpBuf[t]` = popped frame, clear `ValidJmpBuf[t]`. |
| `tm_commit()` / tx end | Clear `InTx[t]`. |

### Invariant

```
JmpBufSafe ==
    \A t \in Thread :
        (InTx[t] = TRUE) => ValidJmpBuf[t] # NONE
```

Violation: a thread is in a transaction but its `jmp_buf` frame has already
returned — a `siglongjmp` would be UB.

## Integration approach

### Option A: Meta-process per backend

Add a single `MetaProc` process that interleaves with `ThreadProc`:
- Wraps every `ThreadProc` step with a meta-step that checks frame validity.
- Pro: no changes to existing backend models.
- Con: ~2× state space (double the steps). Each `ThreadProc` action gets a
  matching meta check.

### Option B: Inline per backend

For each backend, add `ValidJmpBuf[t]` and `InTx[t]` to `VARIABLES`, insert the
meta-transitions directly into the PlusCal `L_begin`/`L_commit`/`L_abort` labels.
- Pro: no separate process, state space increase is smaller (~30%).
- Con: modifies every backend model; error-prone.

### Option C: PlusCal macro

Write a reusable `JmpBufTracking` macro that expands inline at each label:
```
macro jmpbuf_begin() begin
    ValidJmpBuf[self] := frame_id;
    InTx[self] := TRUE;
end macro;
macro jmpbuf_end() begin
    InTx[self] := FALSE;
end macro;
```
All backends call `jmpbuf_begin()` in `L_begin` and `jmpbuf_end()` in commit/abort.
- Pro: reusable, minimal per-backend changes.
- Con: requires `frame_id` assignment; frame return not tracked (stack pop is
  implicit in thread exit).

## Recommended: Option A (meta-process)

Start with a single `MetaProc` that runs alongside any backend. This keeps the
existing backend models pristine and demonstrates the concept without touching
18 files.

### Sketch

```
process MetaProc = 0
variables
    FrameStack = [t \in Thread |-> <<>>],
    ValidJmpBuf = [t \in Thread |-> NONE],
    InTx = [t \in Thread |-> FALSE];
begin
L_meta:
    (* Check safety invariant *)
    assert \A t \in Thread : (InTx[t] => ValidJmpBuf[t] # NONE);
    
    either (* sigsetjmp: begin transaction *)
        with (t \in Thread, fid \in FrameId) {
            ValidJmpBuf' := [ValidJmpBuf EXCEPT ![t] = fid];
            InTx' := [InTx EXCEPT ![t] = TRUE];
            FrameStack' := [FrameStack EXCEPT ![t] = Append(FrameStack[t], fid)]
        }
    or   (* siglongjmp: abort / commit *)
        with (t \in Thread) {
            assert ValidJmpBuf[t] # NONE;
            ValidJmpBuf' := [ValidJmpBuf EXCEPT ![t] = NONE];
            InTx' := [InTx EXCEPT ![t] = FALSE]
        }
    or   (* frame return *)
        with (t \in Thread) {
            FrameStack[t] # <<>>;
            FrameStack' := [FrameStack EXCEPT ![t] = RemoveLastElement(FrameStack[t])]
        };
    goto L_meta
end process
```

### Limitations

- Frame ID generation requires unbounded `FrameId` set (or bounded for TLC).
- The meta-process interleaves arbitrarily — may produce false-positive
  violations where the meta-step fires between backend steps in a way the real
  C++ never does (e.g., `sigsetjmp` meta-step fires while thread is mid-commit).
- Frame return tracking needs a C stack model (not just transaction state).
- The model assumes `siglongjmp` is the only way to exit a transaction; in
  plugin mode `tm_commit()`/`tm_abort()` also exit without longjmp.

## Priority: P3 (low)

All 18 backends already have defense-in-depth against this UB:
1. Primary `tm_jmpbuf` is `__thread` TLS — cannot dangle
2. `tm_set_jmpbuf()` refreshes backend pointer after every `sigsetjmp()`
3. Stack-local jmpbufs used within same function scope
4. Plugin mode injects `sigsetjmp` only on outermost transaction entry

The meta-invariant would formalize what the code already guarantees. Worth
doing for completeness, but not urgent.
