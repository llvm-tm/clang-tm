use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use tm::TmCell;

const NUM_CELLS: usize = 100;
const ITERS: u64 = 1000;

fn single_tx() {
    let cells: Vec<TmCell<u64>> = (0..NUM_CELLS).map(|i| TmCell::new(i as u64)).collect();
    tm::tm_init();
    tm::tm_init_thread();
    for _ in 0..ITERS {
        tm::transaction(|tx| {
            for cell in cells.iter() {
                let v = tx.read(cell);
                tx.write(cell, v.wrapping_add(1));
            }
        });
    }
    println!(
        "single_tx: {} iters, {} cells, {} commits, {} aborts",
        ITERS,
        NUM_CELLS,
        tm::tm_commit_count(),
        tm::tm_abort_count()
    );
    tm::tm_exit_thread();
    tm::tm_exit();
}

fn multi_tx(num_threads: usize) {
    let cells = Arc::new(
        (0..NUM_CELLS).map(|i| TmCell::new(i as u64)).collect::<Vec<_>>(),
    );
    let stop = Arc::new(AtomicBool::new(false));
    let mut handles = Vec::new();
    tm::tm_init();
    for _ in 0..num_threads {
        let cells_ref = Arc::clone(&cells);
        let stop_ref = Arc::clone(&stop);
        handles.push(thread::spawn(move || {
            tm::tm_init_thread();
            while !stop_ref.load(Ordering::Relaxed) {
                tm::transaction(|tx| {
                    for cell in cells_ref.iter() {
                        let v = tx.read(cell);
                        tx.write(cell, v.wrapping_add(1));
                    }
                });
            }
            tm::tm_exit_thread();
        }));
    }
    thread::sleep(std::time::Duration::from_secs(2));
    stop.store(true, Ordering::Relaxed);
    for h in handles {
        h.join().unwrap();
    }
    println!(
        "multi_tx({} threads): {} commits, {} aborts",
        num_threads,
        tm::tm_commit_count(),
        tm::tm_abort_count()
    );
    tm::tm_exit();
}

fn main() {
    single_tx();
    tm::tm_reset_stats();
    multi_tx(2);
}
