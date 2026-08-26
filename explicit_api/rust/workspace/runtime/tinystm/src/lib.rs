// ── C++ vs Rust TinySTM abort mechanics ──────────────────
//
// C++ TinySTM uses siglongjmp for abort: when a conflict is detected
// (in read_word or write_word), abort_tx() releases locks, clears
// state, and calls siglongjmp(*jmpbuf, 1). Control returns to the
// sigsetjmp recovery point in the retry loop (tm_api.hpp:141 or
// tx_executor.hpp:65). No further TM operations execute — the
// transaction body is abandoned immediately. This is the optimal
// strategy: zero wasted work on a doomed transaction.
//
// Rust TinySTM uses a cooperative flag-based abort pattern instead:
//
//   1. Conflict detected → tx.aborted = true  (read_word, write_word)
//   2. write_word checks   if tx.aborted { return; }    at top
//      (avoids useless lock waits / version checks)
//   3. tm_commit() checks  if tx.aborted { unlock; return false; }
//      → retry loop in transaction() calls tm_begin() again
//
// This design is forced by Rust's safety model:
//   - No siglongjmp equivalent: there is no safe way to unwind the
//     call stack + thread-local state without running destructors.
//     Rust's panic/catch_unwind runs destructors (which would free
//     reborrowed RefCell state), making it unsuitable for hot-path
//     TM abort — it would need every closure to be UnwindSafe.
//   - no_std compatible: the flag-based pattern requires only
//     integer CAS + RefCell, no libstd unwinding machinery.
//
// Performance implication:
//   C++  siglongjmp  → instant abort, no wasted TM work
//   Rust lazy-abort  → full transaction body runs (~200 TM ops),
//                       then tm_commit() returns false, retry loop
//                       restarts. At 54% abort rate, ~54% of all
//                       TM operations are wasted.
//
//   Measured overhead (2 threads, 2s, 100 shared cells, 3-run avg):
//     C++  TinySTM wbctl:  133,684 commits  (34% abort rate)
//     Rust TinySTM wbctl:   72,690 commits  (54% abort rate)
//     Ratio: 1.84×  (reduced from 50× after fixing gc_acquire CAS
//     bottleneck and read-set duplication).
//
//   The remaining 1.84× gap is dominated by lazy-abort vs longjmp.
//   An early tx.aborted check in read_word was tried but removed:
//   it adds a with_tx (RefCell borrow) overhead on every successful
//   read, making the common case slower.
//
// Alternative abort mechanisms in other Rust TM backends:
//   - tl2, dudetm, norec: use panic_any(TmxAbort) + catch_unwind
//     in transaction(). This unwinds past the closure immediately,
//     avoiding the "zombie window" where subsequent TM ops would
//     run on aborted state. Drawback: requires the closure body to
//     be UnwindSafe, prevents FFI frames on the stack, and allocates
//     a panic payload on every abort.
//   - TinySTM family (this crate): lazy-abort flag (no panic).
//     Simpler, no unwind safety issues, no allocation on abort.
//     The zombie window is harmless here: write_word checks
//     tx.aborted and returns early; read_word sets tx.aborted
//     and returns a raw value that will never be committed.
//
// Allow dead code and unused imports when this crate is compiled but no TinySTM
// variant is active (e.g., when building with --features tm/norec).
#![allow(dead_code, unused_imports)]

mod common;

// Only compile the selected variant's module.
// When *both* the default (wbctl) and an explicit variant (wbetl/wt) are
// active (e.g. via `--features wt` on top of defaults), the explicit one wins.
#[cfg(all(feature = "wbctl", not(any(feature = "wbetl", feature = "wt"))))]
mod wbctl;
#[cfg(all(feature = "wbetl", not(feature = "wt")))]
mod wbetl;
#[cfg(feature = "wt")]
mod wt;

// Re-export the selected variant.
#[cfg(all(feature = "wbctl", not(any(feature = "wbetl", feature = "wt"))))]
pub use wbctl::*;
#[cfg(all(feature = "wbetl", not(feature = "wt")))]
pub use wbetl::*;
#[cfg(feature = "wt")]
pub use wt::*;

pub use common::{tm_init, tm_exit, tm_begin, tm_init_thread, tm_exit_thread, tm_abort_count, tm_commit_count, tm_reset_stats, TxState, WriteEntry};
#[cfg(feature = "simulation")]
pub use common::sim;
