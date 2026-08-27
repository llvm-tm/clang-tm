// Companion listing: the bank transfer in safe Rust.
// Source: adapted from explicit_api/rust/workspace/tm/examples/simple.rs
// This file compiles against the `tm` crate: cargo build --example bank_transfer
use tm::{TmCell, transaction, tm_init, tm_exit};
use std::sync::Arc;

fn main() {
    tm_init();

    let a = Arc::new(TmCell::new(100_i64));
    let b = Arc::new(TmCell::new(100_i64));

    // Both reads and both writes are one atomic unit.
    let ok = transaction(|tx| {
        let bal_a = tx.read(&a);
        let bal_b = tx.read(&b);
        if bal_a < 10 {
            return false; // floor check: abort, no effect at all
        }
        tx.write(&a, bal_a - 10);
        tx.write(&b, bal_b + 10);
        true
    });
    assert!(ok);

    let total = transaction(|tx| tx.read(&a) + tx.read(&b));
    assert_eq!(total, 200);

    tm_exit();
}
