# `tm-access-trace` — LLVM pass plan

Elegant alternative to debug-patch tracing of **transactional accesses**: a
standalone LLVM pass that injects a per-access trace event into
*already-instrumented* IR, so you can trace any instrumented `.bc` without
re-running (or re-building) the big `libTMInstrument` pipeline.

## Why a separate pass (vs `--emit-tm-trace`)

The instrument pass can already emit `tm_trace(...)` at each tracked access
when `--emit-tm-trace` is set (`plugin/passes/tm_runtime_hooks.hpp`,
`emitTMRead`/`emitTMWrite`). But that couples tracing to the instrumentation
build. `tm-access-trace` decouples them: it is a small, fast-to-iterate
standalone `.so` (`libTMAccessTrace.so`) that runs over any instrumented module
and reuses the *existing* runtime sink — no new runtime code.

## Reuse / common code

- `plugin/passes/tm_runtime_hooks.hpp` — `TMRuntimeHook`, `declareHook`,
  `emitHookCall`, and the `tm_trace` hook (`trace_fn`). The pass emits the same
  `tm_trace(uint32 type, void* addr, uint64 width, uint64 value)` shape as
  `--emit-tm-trace`, so the existing sink (`backends/tm_impl/common/tm_trace_runtime.cpp`)
  and the simulator (`simulator/`, `tm-trace2jsonl`) consume it unchanged.
  Trace type codes (sink contract): `0=read`, `1=write`, `7=computation`.
- `plugin/passes/tm_access_hooks.hpp` (**new, shared**) — pure classification of
  an IR call as a TM access hook (`Read`/`Write`), giving the address operand,
  value operand, and byte width. Used by `tm-access-trace`; also usable by the
  race checker / instrumentation to replace their duplicated hook-name switches.

## Pass behavior

For each `tm_read_*`/`tm_write_*` hook call in the module, insert immediately
before it:
```
tm_trace(type, (void*)addr, width, value)   // type: read=0, write=1
```
- `addr` = first pointer arg (bitcast to `i8*`).
- `width` = access width in bytes, derived from the hook name (`tm_read_i4`→4,
  `tm_write_f8`→8, `tm_write_ptr`→8, `tm_read_i16`→16, …).
- `value` = second arg for writes (extended/bitcast to `i64`); `0` for reads.
- Idempotent: skips calls that are themselves `tm_trace`. Runs only on
  instrumented IR (no-op if no hook calls present).

## Phases

- [x] **P0 — plan + tests + pass + build** (this change):
  shared `tm_access_hooks.hpp`; `TMAccessTracePass.cpp` (standalone
  `libTMAccessTrace.so`, pipeline name `tm-access-trace`); Makefile target;
  tests `tests/plugin/test_access_trace.cpp` + `tests/plugin/run_access_trace_test.sh`
  (IR-level check that `tm_trace` calls are injected + end-to-end check that the
  trace file has the expected read/write event counts).
- [ ] **P1 — wire into `clang-tm`**: add a `--access-trace` convenience flag that
  appends `tm-access-trace` after the instrument step; add a `make` target.
- [ ] **P2 — refactor to reuse the header**: switch `TMRaceCheckerPass` /
  `tm_runtime_hooks` access switches to call `classifyTMAccess` (dedup).
- [ ] **P3 — begin/end events**: optionally inject `tm_trace(2/3,...)` at
  `tm_begin`/`tm_end` so traces carry transaction boundaries (today only the
  instrumentation emits those on the queue path).
- [ ] **P4 — analysis**: a small reader (`tm-trace2jsonl` already converts) +
  counts/abort cross-check to spot "tracked_write=0" style bugs like B-21.

## Build / test

```sh
make -C plugin bin/libTMAccessTrace.so
bash tests/plugin/run_access_trace_test.sh        # IR + end-to-end
# manual:
opt-22 -load-pass-plugin=plugin/bin/libTMAccessTrace.so -passes=tm-access-trace \
     in.instr.bc -S -o out.ll
```

## See also
[DEBUGGING_BACKENDS.md](DEBUGGING_BACKENDS.md), [INSTRUMENTATION_DEBUGGING.md](INSTRUMENTATION_DEBUGGING.md),
[`../simulator/README.md`](../simulator/README.md).
