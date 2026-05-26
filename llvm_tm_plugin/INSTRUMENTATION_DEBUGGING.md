# Debugging Missing TM Instrumentation

## Overview

When a TM-annotated program produces incorrect results (money non-conservation,
data races, hangs), the root cause is often **missing instrumentation**: a load
or store inside a transaction function that bypasses `tm_read`/`tm_write` and
accesses memory directly.

This document describes a methodology for detecting and fixing missing
instrumentation, from quick static checks to runtime analysis with valgrind.

## Quick Checks

### 1. Static verification: `tm-instrument-check`

Append `-passes="tm-instrument-check"` to any pipeline to scan post-instrumentation
IR for raw loads/stores to non-local, non-`tm_local` addresses in TX functions:

```sh
# Using clang-tm with the check pass appended
clang-tm --runtime TinySTM_runtime.cpp -o myapp myapp.cpp \
  -Wl,--plugin-arg=opt:-passes="tm-instrument,tm-instrument-check"
```

If any uninstrumented access is found, the plugin exits with an error listing
file, line number, and the offending instruction:

```
TM-CHECK: UNINSTRUMENTED STORE in _Z8transferiii
  store i32 %17, ptr %18, align 4
  at bank.cpp:95:9
TM-CHECK: FAIL - _Z8transferiii has uninstrumented loads/stores
error: TM instrumentation check failed. Use -tm-allow-opaque to suppress.
```

The check pass is safe for CI — it validates every TX-function load/store,
skipping allocas and `tm_local` variables.

### 2. Manual IR inspection

Compare the uninstrumented vs instrumented IR:

```sh
# Generate both
clang-tm --keep-temps --runtime TinySTM_runtime.cpp -o /tmp/app app.cpp

# Check for typed TM calls
llvm-dis-22 out/app.instr.bc -o /tmp/instr.ll
grep -c 'tm_read_i\|tm_write_i' /tmp/instr.ll

# List all loads/stores in a specific TX function
grep -A 50 'define.*_Z8transferiii' /tmp/instr.ll | grep -E 'load|store|tm_read|tm_write'
```

Expected output for a properly instrumented function:

```
  %18 = call i32 @tm_read_i4(ptr %17)     # ← instrumented load
  call void @tm_write_i4(ptr %20, i32 %19) # ← instrumented store
```

A raw `load i32` or `store i32` without a preceding `tm_read_`/`tm_write_`
call indicates missing instrumentation.

### 3. Plugin audit mode (`-tm-audit`)

The plugin has a built-in audit mode that enumerates every load/store in TX
functions, classifying each as instrumented or not:

```sh
# Build debug variant
make bin/libTMInstrument_debug.so

# Run instrumentation with audit
opt-22 -load-pass-plugin=bin/libTMInstrument_debug.so \
  -passes="tm-instrument" -tm-audit out/app.bc -o /dev/null 2>&1
```

The audit output lists every load/store with classification:

```
[AUDIT] === ALL loads in _Z8transferiii ===
[AUDIT]   LOAD tm_local=N type=i32
    ptr=  %15 = tail call ptr @llvm.ptr.annotation.p0.p0(...)
    base= %15 = tail call ptr @llvm.ptr.annotation.p0.p0(...)
[AUDIT]   LOAD tm_local=N type=i32
    ptr=  %20 = tail call ptr @llvm.ptr.annotation.p0.p0(...)
    base= %20 = tail call ptr @llvm.ptr.annotation.p0.p0(...)
[AUDIT] Summary for _Z8transferiii: LOAD 6/6 STORE 4/4 INSTRUMENTED
```

Look for lines where the instrumented count is less than the total count.

## Runtime Detection with Valgrind

### What valgrind detects

Missing TM instrumentation means a store inside a transaction writes directly to
memory without going through the TM write-set. If the transaction later aborts,
the write-back TM runtime cannot undo it (no undo log entry), leaving stale
data visible to non-transactional readers.

Valgrind tools detect different aspects of this:

| Tool | Detects | Signals missing instrumentation when |
|---|---|---|
| `memcheck` | Uninitialized reads, use-after-free | A non-TM write persists after abort; a TM read returns stale value |
| `helgrind` | Data races | A non-TM access races with another thread's TM or non-TM access |
| `drd` | Data races (lighter) | Same as helgrind, lower overhead |

### Methodology: incremental race detection

The idea is to compare the race profile of the instrumented binary against
a known-correct baseline (e.g., `SingleGlobalLock` runtime, which serializes
all transactions and is trivially correct).

#### Step 1: Run with SingleGlobalLock (expected: clean)

```sh
# Build with SingleGlobalLock runtime
make -C benchmarks/test/bank bank_singlelock

# Run under helgrind
valgrind --tool=helgrind \
  --log-file=/tmp/helgrind_sgl.log \
  ./benchmarks/test/bank/bin/bank_singlelock -t 4 -d 1000
```

The SingleGlobalLock backend serializes all TX execution within a global mutex.
Any race reported here is a bug in the application, not TM instrumentation.

#### Step 2: Run with TM backend (look for NEW races)

```sh
# Build with a real TM backend
make -C benchmarks/test/bank bank_tinystm

# Run under helgrind
valgrind --tool=helgrind \
  --log-file=/tmp/helgrind_tm.log \
  ./benchmarks/test/bank/bin/bank_tinystm -t 4 -d 1000
```

Compare the two log files. NEW races in the TM backend (not present in
SingleGlobalLock) indicate missing TM instrumentation.

#### Step 3: Correlate races to source locations

```sh
grep 'Conflict' /tmp/helgrind_tm.log | head -20
```

Typical output:

```
==1234== Possible data race during write of size 4 at 0x... by thread 3
==1234==    at 0x...: transfer(int, int, int) (bank.cpp:95:9)
```

The line number points to the uninstrumented store. Cross-reference with the
instrumented IR to confirm.

### Using DRD for lower overhead

DRD is lighter than helgrind and better for longer-running benchmarks:

```sh
valgrind --tool=drd --exclusive-threshold=100 \
  --log-file=/tmp/drd_tm.log \
  ./bin/myapp -t 4
```

DRD reports races with less memory overhead, making it practical for
benchmarks that run millions of transactions.

### Interpreting valgrind output

**False positives** are common with valgrind and TM due to:
- **Lock-based internal synchronization** in the runtime (e.g., lock table
  access): suppress with `--suppressions=tm_suppressions.supp`
- **Atomic operations used by the runtime** (e.g., `fetch_add`): valgrind
  doesn't always understand C++ atomics. Add suppressions for known-safe
  runtime functions.

**True positives** have the signature:
1. A write by one thread inside a TX function
2. A concurrent read/write by another thread (possibly also in a TX function)
3. The access is NOT through `tm_read`/`tm_write` (confirmed by IR inspection)

#### Creating a suppressions file

```tm_suppressions
{
   <insert_a_suppression_name_here>
   Helgrind:Race
   fun:tm_*
}
{
   <insert_a_suppression_name_here>
   Helgrind:Race
   fun:std::*
}
```

### Step-by-step: finding a missing instrumented store

1. Run with SingleGlobalLock under helgrind → save log as baseline
2. Run with target TM backend under helgrind → save log
3. Diff the logs to find new races specific to the TM backend
4. For each new race, locate the source line
5. Check the corresponding LLVM IR for instrumentation:
   ```sh
   grep -B5 -A5 'bank.cpp:95' /tmp/instr.ll
   ```
6. If it's a raw `load`/`store` without `tm_read_`/`tm_write_`, the
   instrumentation is missing

## Common Root Causes

### 1. `hasCloneInstrumentation` guard (removed in f35d19e)

The `TMInstrumentPass` previously skipped ALL `handleLoadStore` when the TX
function contained any `tm_read_*`/`tm_write_*` call from inlined instrumented
clones. This was overly conservative — it also skipped loads/stores that were
never part of any clone (e.g., struct field access in the TX function body
itself).

**If you see `tm_read_ptr` calls but NO `tm_read_i4`/`tm_write_i4` calls** for
struct fields loaded/stored directly in the TX function, this guard may have
been reintroduced. Check `TMInstrumentPass.cpp` for a `hasCloneInstrumentation`
check around line 500.

### 2. Alloca misclassification (getBaseObject vs getBaseObjectNoLoad)

`handleLoadStore` uses `getBaseObject` (which traces through `LoadInst`) to
determine if a pointer targets an alloca. When a pointer loaded from an alloca
points to heap memory, `getBaseObject` traces through the `LoadInst` and
returns the alloca itself, falsely marking the access as local.

**Symptom:** loads/stores to heap memory that trace through an alloca-loaded
pointer are not instrumented.

**Fix:** switch to `getBaseObjectNoLoad` (which stops at `LoadInst`). See
`tm_instrument_helpers.hpp` for the detailed comment and empirical trade-offs.

### 3. `@llvm.ptr.annotation` confusion

`TM int balance` generates `@llvm.ptr.annotation` calls wrapping the field
address. The `getBaseObject` function should `stripPointerCasts()` through
these annotations to reach the underlying GEP. In LLVM 22+, `stripPointerCasts`
may NOT strip `@llvm.ptr.annotation`, causing `getBaseObject` to return the
annotation call itself rather than the underlying allocation.

**Symptom:** `getBaseObject` returns a `CallInst` for annotated fields, which
is not an `AllocaInst`, so instrumentation proceeds. This path is correct in
the current codebase.

### 4. Memory intrinsic bypass

`memcpy`/`memmove`/`memset` calls inside TX functions bypass normal
load/store instrumentation. They are handled separately by
`instrumentMemoryIntrinsic`. If that function doesn't cover your case (e.g.,
a type-suffixed intrinsic name in LLVM 22), the byte-level writes won't be
instrumented.

**Symptom:** `memcpy` or `memset` inside a TX creates raw `load`/`store`
instructions at the byte level after expansion.

**Fix:** Add the missing intrinsic name pattern to
`needsMemIntrinsicInstrumentation` and `instrumentMemoryIntrinsic` in
`tm_method_instrumentation.hpp`.

### 5. Pipeline selection

The `clang-tm` wrapper selects the pipeline by default based on the
optimization level:

| Pipeline | Default | Behavior |
|---|---|---|
| `tm-instrument` | `-O0` (DEBUG) | CloneOnly + NoInline/OptimizeNone clones; TX function instrumented separately |
| `tm-instrument-then-inline` | release builds | AlwaysInline clones, instrument individually, then inline; TX function instrumented with hasCloneInstrumentation |
| `tm-instrument-inline` | never (opt-in) | AlwaysInline clones, inline, then TMInstrumentInlinePass instruments everything post-inline |

**If a pipeline change causes instrumentation regression, compare the
instrumented IR side-by-side:**

```sh
# Build with both pipelines
clang-tm -passes="tm-instrument" --keep-temps -o /tmp/a app.cpp
clang-tm -passes="tm-instrument-then-inline" --keep-temps -o /tmp/b app.cpp

# Compare
diff <(grep 'tm_read_\|tm_write_' /tmp/a.instr.ll | sort) \
     <(grep 'tm_read_\|tm_write_' /tmp/b.instr.ll | sort)
```

## Test Harness for Instrumentation Correctness

### Money-conservation benchmark (`benchmarks/test/bank/bank.cpp`)

The bank benchmark is the primary correctness litmus test. It verifies that
the sum of all account balances remains constant across concurrent transfers.
**Any deviation from the initial total = missing instrumentation or a TM
correctness bug.**

```sh
make -C benchmarks/test/bank bank_tinystm
./benchmarks/test/bank/bin/bank_tinystm -t 4 -d 5000
# Expected: ✓ PASS: Bank total is correct (money was conserved)
```

To stress-test at higher thread counts:

```sh
for t in 1 2 4 8 16; do
  for i in 1 2 3; do
    ./bin/bank_tinystm -t $t -d 2000 | grep -E 'PASS|FAIL'
  done
done
```

### The `test_local_containers` regression check

This test exercises `std::vector` and `std::map` inside transactions. If the
`hasCloneInstrumentation` guard is incorrectly modified, the abort rate may
increase:

```sh
BUILD_TYPE=DEBUG make test_local_containers
./bin/test_local_containers 4 100 100 2>&1 | tail -5
# Check abort count is reasonable (single-digit percentage)
```

### CI integration

Run the check pass in CI after every build:

```makefile
check-instrumentation:
    @for bc in out/*.instr.bc; do \
        echo "Checking $$bc..."; \
        opt-22 -load-pass-plugin=bin/libTMInstrument.so \
          -passes="tm-instrument-check" $$bc -o /dev/null 2>&1 || exit 1; \
    done
```

## Summary Workflow

```
1. Run bank benchmark → money conservation?
   NO  → Compare instrumented IR for missing tm_read_i4/tm_write_i4
       → Check hasCloneInstrumentation guard
       → Check getBaseObject vs getBaseObjectNoLoad
       → Run TMInstrumentCheckPass in CI

2. Run valgrind/helgrind → races not in SingleGlobalLock baseline?
   YES → Locate the uninstrumented store from the race report
       → Check the corresponding LLVM IR for missing tm_read/tm_write
       → Identify why handleLoadStore skipped it (alloca? annotation? type?)

3. Verify fix:
   - Bank benchmark PASSES at 2t/4t
   - TMInstrumentCheckPass reports zero uninstrumented loads/stores
   - No new races in helgrind vs baseline
```
