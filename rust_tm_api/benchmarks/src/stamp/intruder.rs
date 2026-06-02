use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Intruder ===");
    println!("  Packets: {}  Atoms: {}  Max length: {}",
             config.num_packets, config.num_atoms, config.max_length);

    let hash_size: usize = 1 << 20;
    let hash_table: Vec<TmCell<u32>> = (0..hash_size).map(|_| TmCell::new(0)).collect();
    let table = Arc::new(hash_table);
    let dur = std::time::Duration::from_millis(config.duration as u64);
    let g_inserts = AtomicU64::new(0);
    let g_deletes = AtomicU64::new(0);

    let t0 = std::time::Instant::now();
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let ht = table.clone();
            let sc = stop;
            let so = ops.clone();
            let ins = &g_inserts;
            let del = &g_deletes;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                let atoms: Vec<(u32, u32)> = (0..config.num_atoms)
                    .map(|i| {
                        let len = (rng.borrow_mut().next() % config.max_length as u64) as u32 + 1;
                        let val = (rng.borrow_mut().next() % 10000) as u32;
                        (val, len)
                    }).collect();

                while !sc.load(Ordering::Relaxed) {
                    let is_insert = rng.borrow_mut().next() % 2 == 0;
                    let atom_idx = (rng.borrow_mut().next() % config.num_atoms as u64) as usize;
                    let (val, _len) = atoms[atom_idx];
                    let bucket = (val as usize) % hash_size;

                    transaction(|tx| {
                        if is_insert {
                            tx.write(&ht[bucket], val);
                        } else {
                            let _found = tx.read(&ht[bucket]) == val;
                        }
                    });

                    if is_insert { ins.fetch_add(1, Ordering::Relaxed); }
                    else { del.fetch_add(1, Ordering::Relaxed); }
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;
    let inserts = g_inserts.load(Ordering::Relaxed);
    let deletes = g_deletes.load(Ordering::Relaxed);
    println!("  Inserts: {}  Deletes: {}  Elapsed: {} ms", inserts, deletes, elapsed);
}
