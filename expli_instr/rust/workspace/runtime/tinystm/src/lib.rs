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

pub use common::{tm_init, tm_exit, tm_begin, tm_init_thread, tm_exit_thread, tm_abort_count, TxState, WriteEntry};
#[cfg(feature = "simulation")]
pub use common::sim;
