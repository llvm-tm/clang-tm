# Simulator Issues & Improvement Plan

## Current Problems

### P0 — Crash/Hang

1. **Stack overflow in tm-live** (`simulator/` issues)
   - Running `tm-live` with `social_tm_live.so` crashes: `thread 'main' has overflowed its stack`
   - Root cause unknown. Hypothesis: the Rust FFI → C++ SimBackend → app .so → TM operations → Rust FFI call chain recurses infinitely.
   - The `[tm-live] calling app entry point...` diagnostic prints once then the process overflows.
   - Need to add frame-pointer diagnostics or run under lldb to find the exact call path.

2. **DATA/TEXT symbol conflict for TM hooks** (shared-library ABI)
   - App .so references `tm_*` as DATA symbols (function pointers).  SimBackend .so defines them.
   - macOS flat namespace (`-flat_namespace`) + `RTLD_GLOBAL` required for resolution — fragile and platform-specific.
   - Cross-platform: `-flat_namespace` is macOS-only; Linux uses `-Wl,--allow-shlib-undefined` differently.
   - Alternative: compile the TM hook DATA variables into a thin stub that both .so files link (but then each has its own copy).

3. **TLS variable sharing across shared libraries** (`__thread`)
   - `tm_longjmp_ret`, `tm_nested_call_counter`, `tm_jmpbuf` are `__thread` variables defined in SimBackend but accessed directly by app .so.
   - TLS access across `dlopen`'d libraries is platform-specific and fragile.
   - macOS with flat namespace: may work but untested for multi-threaded case.
   - Linux: `__thread` in shared libraries works with `RTLD_GLOBAL` but requires careful ordering.

4. **sigsetjmp/siglongjmp retry mechanism is broken**
   - `sim_sigsetjmp()` calls real `sigsetjmp` on the app's `tm_jmpbuf`, but this saves the context of the **SimBackend** function frame, not the app's `transaction()` template frame.
   - When `sim_end()` calls `siglongjmp`, it restores to inside `sim_sigsetjmp` (which then returns 1), effectively simulating a retry — **but this requires the stack of `sim_sigsetjmp` to still be valid**.
   - If `sim_sigsetjmp` had returned before `siglongjmp` is called, the saved context is invalid (stack corruption).  This is undefined behaviour.
   - Correct approach: save/restore only the **program state** (read-set, write-set, PC), not the CPU registers via `sigsetjmp`.

### P1 — Correctness

5. **No conflict detection** (`sim_state.rs`)
   - `tm_end` always returns `true` — every transaction commits regardless of conflicts.
   - Read-set is collected but never validated.  Write-set is written to shadow memory unconditionally.
   - Need OCC-style validation: on `tm_end`, check each read-set entry's address in shadow memory; if value changed since read, abort.

6. **Non-transactional writes silently dropped during init**
   - App initializes globals outside a transaction (e.g., `g_nodes[i].post_count = 0` in `init_graph()`).
   - The LLVM pass instruments these writes through `tm_write_i8`, but `sim_state::tm_write` silently drops them because `!t.in_tx`.
   - Shadow memory starts from all-zeros, so transactions read 0 for all fields regardless of init values.
   - Fix: either (a) write-through to shadow even when not in a transaction, or (b) run init inside a transaction.

7. **Follow/unfollow invariant fails with 0 aborts** (DeathStarBench)
   - `sum(follower_count) == sum(following_count)` fails even with 1 thread and 0 simulated aborts.
   - Root cause: `g_nodes` is allocated via `new SocialNode[n]()` (regular heap), not `tm_malloc`.  TM operations on heap addresses work through the SimBackend, but the non-transactional init writes are dropped (see #6).
   - After the first transaction, the shadow memory has partial state, and subsequent transactions can produce non-idempotent increments.

8. **Shadow memory never initialized** (no initial state)
   - App expects globals to start at 0 (zero-initialized by OS for BSS).  Shadow memory starts empty (`HashMap::new()`) — reads of uninitialised addresses return 0, which matches BSS semantics accidentally.
   - But heap-allocated structs (via `new[]`) have their initial values set outside a transaction, so shadow never sees them.

### P2 — Completeness

9. **Multi-threaded execution not supported**
   - Thread 0 (main thread) works; worker threads lack proper thread-local state management.
   - `Barrier` synchronisation in the app calls `tm_init_thread` from worker threads, but the SimBackend's per-thread ID assignment via `static atomic<uint32_t> next_id` is fragile.
   - Mutex in `with_state()` would be a bottleneck for multi-threaded simulation.
   - Need per-thread lock-free state or sharded mutexes.

10. **No clock/cycle accounting**
    - `self.clock` is incremented by hardcoded constants (60 for begin, 5 for read, etc.) — not calibrated against real hardware.
    - No modelling of:
      - Cache misses
      - Memory latency differences
      - TSX vs non-TSX costs
      - Lock contention costs
    - Without realistic cycle accounting, the simulation cannot predict real throughput.

11. **Architecture mismatch between Rust and C++ toolchains**
    - Rust toolchain is `x86_64-apple-darwin` (Rosetta) while C++ compiler produces `arm64`.
    - Workaround: `-target x86_64-apple-darwin` flag on C++ compiler.
    - Ideally: align both to native `aarch64-apple-darwin`.

### P3 — Maintainability

12. **Two separate build systems** (C++ Makefile + Rust Cargo) for a single pipeline.
13. **SimBackend .so path hardcoded relative to binary** — fragile; requires absolute paths at CLI.
14. **No test suite for live-app simulation** — no automated regression tests.

## Improvement Plan

### Phase 1A — Fix the stack overflow (P0)

- **Replace sigsetjmp/siglongjmp with lazy-abort simulation.**
  - `sim_sigsetjmp` saves the app's jmpbuf pointer, returns 0.
  - `sim_end` does NOT call `siglongjmp`.  Instead, it returns a success/failure code.
  - The SimBackend sets `tm_longjmp_ret = 1` when commit fails.
  - The app's `transaction()` template checks `tm_longjmp_ret` (it currently does NOT — see below).
  - **If the app's generated code doesn't check `tm_longjmp_ret`**, we need to modify the retry loop to poll.

- **Simplify the retry model: make all transactions succeed (single-threaded).**
  - Comment: "single-threaded no-conflict mode: all commits succeed".
  - This lets us verify the end-to-end pipeline works before adding conflict detection.

### Phase 1B — Wire-through to verify end-to-end (P1)

- **Add write-through for non-transactional operations:**
  - `sim_state::tm_write` when `!t.in_tx`: write directly to shadow memory.
  - This ensures init values are captured.

- **Test with social_tm `--test follow-unfollow`:**
  - Single-threaded, all commits succeed, shadow memory tracks all writes.
  - Verify `sum(follower) == sum(following)` invariant passes.

### Phase 2 — Add OCC conflict detection (P1)

- **Read-set validation at tm_end:**
  - For each read-set entry, re-read current value from shadow memory.
  - If value changed since time of read, abort the transaction.
  - Signal abort to the app's retry loop.

- **Write-set conflict detection (for multi-threaded Phase 3):**
  - Track write-set overlap between threads.
  - On concurrent write to overlapping addresses, abort one.

### Phase 3 — Multi-threaded simulation (P2)

- **Per-thread state without mutex:**
  - Use `thread_local!` for per-thread state, not a global `Mutex<LiveSimState>`.
  - Shadow memory still needs synchronisation — use `RwLock<HashMap>` and stripe locking.

- **Deterministic scheduling:**
  - Co-routine-based interleaving (boost::context or Rust async).
  - Each thread runs as a co-routine; the scheduler chooses interleaving points.

### Phase 4 — Cycle-accurate costing (P2)

- **Calibrated cost tables:**
  - Load `machine_profile.json` for per-backend cycle costs.
  - Use the calibrated TSX cycle costs from the existing profiling infrastructure.

- **Time-multiplexed simulation:**
  - Each TM operation advances a virtual clock.
  - Scheduler uses clock to determine when to switch between co-routines.

### Phase 5 — Cross-platform robustness (P2)

- **Use `dlsym(RTLD_DEFAULT, ...)` for symbol lookup** instead of relying on `RTLD_GLOBAL` + `-flat_namespace`.
  - Alternative: stub `.o` that defines TM hook DATA variables, linked into both SimBackend and app .so.
  - On Linux: use `--Wl,--no-as-needed` to force symbol resolution.

- **Add `aarch64-apple-darwin` Rust target** to avoid Rosetta / `-target` flags.

### Phase 6 — Test suite and CI (P3)

- **Integration test for tm-live:**
  - Build social_tm_live.so, run tm-live, check exit code 0.
  - Run with `--test compose` (workload invariant) in CI.

- **Regression test for DATA/TEXT conflict:**
  - Verify that undefined `tm_*` symbols in app .so resolve to correct values.

## Immediate Next Step

Fix the stack overflow (Phase 1A).  Three options:

**Option A** — Remove `sigsetjmp`/`siglongjmp` from the SimBackend entirely.  In `sim_end`, NEVER call `siglongjmp` (all commits succeed).  Set `tm_longjmp_ret = 1` and let the app's retry loop handle it.  But the current `transaction()` template does NOT check `tm_longjmp_ret` — it just re-calls `sigsetjmp`.  We need to modify the generated code or the template to poll.

**Option B** — Accept that single-threaded simulation is no-conflict.  Make `sim_end` always succeed and never retry.  This is correct for single-threaded traces without concurrent access.  No changes needed to the retry mechanism.

**Option C** — Use real `sigsetjmp`/`siglongjmp` correctly by having `sim_sigsetjmp` actually call `sigsetjmp` on the app's buffer (which saves the APPLICATION's context, not the SimBackend's).  This requires the buffer to be allocated on the app's stack, not in TLS.  Then `sim_end` can call `siglongjmp` to jump back to the app's transaction frame.

Option B is the quickest path to a working end-to-end demo.  Proceed with Option B.
