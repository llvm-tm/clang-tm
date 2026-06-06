use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Vacation ===");
    println!("  Rooms: {}  Cars: {}  Flights: {}  Customers: {}",
             config.num_rooms, config.num_items, config.num_items, config.num_customers);
    let rooms: Vec<TmCell<i32>> = (0..config.num_rooms).map(|_| TmCell::new(100)).collect();
    let cars: Vec<TmCell<i32>> = (0..config.num_items).map(|_| TmCell::new(100)).collect();
    let flights: Vec<TmCell<i32>> = (0..config.num_items).map(|_| TmCell::new(100)).collect();
    let customers: Vec<TmCell<i64>> = (0..config.num_customers).map(|_| TmCell::new(1000)).collect();
    let rm = Arc::new((rooms, cars, flights, customers));
    let dur = std::time::Duration::from_millis(config.duration as u64);

    let g_reserve_ok = AtomicU64::new(0);
    let g_reserve_fail = AtomicU64::new(0);
    let g_cancel_ok = AtomicU64::new(0);
    let g_cancel_fail = AtomicU64::new(0);
    let g_queries = AtomicU64::new(0);

    let t0 = std::time::Instant::now();
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let r = rm.clone();
            let sc = stop;
            let so = ops;
            let ro = &g_reserve_ok;
            let rf = &g_reserve_fail;
            let co = &g_cancel_ok;
            let cf = &g_cancel_fail;
            let q = &g_queries;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                while !sc.load(Ordering::Relaxed) {
                    let choice = rng.borrow_mut().next() % 100;
                    let customer = (rng.borrow_mut().next() % config.num_customers as u64) as usize;
                    let make_rtype = rng.borrow_mut().next() % 3;
                    let make_item = (rng.borrow_mut().next() % 100) as usize;
                    let cancel_rtype = rng.borrow_mut().next() % 3;
                    let cancel_item = (rng.borrow_mut().next() % 100) as usize;

                    let reserved = transaction(|tx| {
                        match choice {
                            0..=39 => {
                                let cost = 50i64;
                                let bal = tx.read(&r.3[customer]) - cost;
                                if bal >= 0 {
                                    tx.write(&r.3[customer], bal);
                                    match make_rtype {
                                        0 => tx.write(&r.0[make_item % config.num_rooms],
                                                       tx.read(&r.0[make_item % config.num_rooms]) - 1),
                                        1 => tx.write(&r.1[make_item % config.num_items],
                                                       tx.read(&r.1[make_item % config.num_items]) - 1),
                                        _ => tx.write(&r.2[make_item % config.num_items],
                                                       tx.read(&r.2[make_item % config.num_items]) - 1),
                                    }
                                    true
                                } else {
                                    false
                                }
                            }
                            40..=79 => {
                                tx.write(&r.3[customer], tx.read(&r.3[customer]) + 50);
                                match cancel_rtype {
                                    0 => tx.write(&r.0[cancel_item % config.num_rooms],
                                                   tx.read(&r.0[cancel_item % config.num_rooms]) + 1),
                                    1 => tx.write(&r.1[cancel_item % config.num_items],
                                                   tx.read(&r.1[cancel_item % config.num_items]) + 1),
                                    _ => tx.write(&r.2[cancel_item % config.num_items],
                                                   tx.read(&r.2[cancel_item % config.num_items]) + 1),
                                }
                                true
                            }
                            _ => { let _ = tx.read(&r.3[customer]); false }
                        }
                    });

                    match choice {
                        0..=39 => {
                            if reserved { ro.fetch_add(1, Ordering::Relaxed); }
                            else { rf.fetch_add(1, Ordering::Relaxed); }
                        }
                        40..=79 => {
                            if reserved { co.fetch_add(1, Ordering::Relaxed); }
                            else { cf.fetch_add(1, Ordering::Relaxed); }
                        }
                        _ => { q.fetch_add(1, Ordering::Relaxed); }
                    }
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;

    let ro = g_reserve_ok.load(Ordering::Relaxed);
    let rf = g_reserve_fail.load(Ordering::Relaxed);
    let co = g_cancel_ok.load(Ordering::Relaxed);
    let cf = g_cancel_fail.load(Ordering::Relaxed);
    let q = g_queries.load(Ordering::Relaxed);
    let txns = ro + rf + co + cf + q;
    let tx_sec = if elapsed > 0 { txns * 1000 / elapsed } else { 0 };
    println!("  Reserve OK: {}  Fail: {}", ro, rf);
    println!("  Cancel  OK: {}  Fail: {}", co, cf);
    println!("  Queries:    {}", q);
    println!("  Total TXNs: {}  TXNs/sec: {}", txns, tx_sec);
}
