use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== SSCA2 ===");
    println!("  Scale: {}  Min degree: {}  Max degree: {}",
             config.scale, config.min_degree, config.max_degree);
    let num_vertices = 1 << config.scale;
    let adj: Vec<TmCell<u8>> = (0..num_vertices).map(|_| TmCell::new(0u8)).collect();
    let adj = Arc::new(adj);
    let dur = std::time::Duration::from_millis(config.duration as u64);
    let g_edges = AtomicU64::new(0);

    let t0 = std::time::Instant::now();
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let a = adj.clone();
            let sc = stop;
            let so = ops;
            let ge = &g_edges;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                while !sc.load(Ordering::Relaxed) {
                    let src = (rng.borrow_mut().next() % num_vertices as u64) as usize;
                    let dst = (rng.borrow_mut().next() % num_vertices as u64) as usize;
                    if src == dst { continue; }

                    let inserted = transaction(|tx| {
                        let cur = tx.read(&a[src]);
                        let cur2 = tx.read(&a[dst]);
                        if cur == 0 && cur2 == 0 {
                            tx.write(&a[src], 1u8);
                            tx.write(&a[dst], 1u8);
                            true
                        } else {
                            false
                        }
                    });

                    if inserted { ge.fetch_add(1, Ordering::Relaxed); }
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;
    let edges = g_edges.load(Ordering::Relaxed);
    println!("  Edges built: {}  TXNs: {}  Elapsed: {} ms",
             edges, ops.load(Ordering::Relaxed), elapsed);
}
