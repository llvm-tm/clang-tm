use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn test() -> i32 {
    let mut fails = 0;
    // Test basic reservation logic (no TM needed)
    let num = 5;
    let mut num_free = num;
    let mut num_used = 0;
    num_free -= 1; num_used += 1;
    if num_free != 4 || num_used != 1 { eprintln!("FAIL: reservation"); fails += 1; }
    num_free += 100;
    if num_free != 104 { eprintln!("FAIL: add inventory"); fails += 1; }
    num_free += 1; num_used -= 1;
    if num_free != 105 || num_used != 0 { eprintln!("FAIL: cancel"); fails += 1; }
    if fails > 0 { eprintln!("vacation: {} test(s) failed", fails); }
    fails
}

pub fn run(config: &Config, stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== Vacation ===");
    let (num_relations, query_range, num_queries_per_tx, percent_user) = {
        let nr = config.num_items.max(1);
        let qr = (nr as f64 * 0.9) as usize;
        (nr, qr.max(1), 2usize, 98)
    };
    println!("  Relations: {}  Query range: {}  Queries/TX: {}  User%: {}",
             num_relations, query_range, num_queries_per_tx, percent_user);

    // Reservation table entry with TM-tracked fields
    struct ResEntry {
        active: TmCell<i32>,
        id: TmCell<i32>,
        num_used: TmCell<i32>,
        num_free: TmCell<i32>,
        num_total: TmCell<i32>,
        price: TmCell<i32>,
    }
    impl ResEntry {
        fn new() -> Self {
            ResEntry {
                active: TmCell::new(0), id: TmCell::new(0),
                num_used: TmCell::new(0), num_free: TmCell::new(0),
                num_total: TmCell::new(0), price: TmCell::new(0),
            }
        }
    }

    // Customer entry
    struct CustEntry {
        active: TmCell<i32>,
        id: TmCell<i32>,
        bill: TmCell<i32>,
    }
    impl CustEntry {
        fn new() -> Self {
            CustEntry { active: TmCell::new(0), id: TmCell::new(0), bill: TmCell::new(0) }
        }
    }

    // Generate initial data — read-only after init
    let mut init_rng = Rng::new(42);
    let cars: Vec<ResEntry> = (0..num_relations).map(|j| {
        let e = ResEntry::new();
        let num = ((init_rng.next() % 5) + 1) as i32 * 100;
        let p_val = ((init_rng.next() % 5) * 10 + 50) as i32;
        unsafe {
            *e.active.ptr() = 1;
            *e.id.ptr() = (j + 1) as i32;
            *e.num_used.ptr() = 0;
            *e.num_free.ptr() = num;
            *e.num_total.ptr() = num;
            *e.price.ptr() = p_val;
        }
        e
    }).collect();
    let rooms: Vec<ResEntry> = (0..num_relations).map(|j| {
        let e = ResEntry::new();
        let num = ((init_rng.next() % 5) + 1) as i32 * 100;
        let p_val = ((init_rng.next() % 5) * 10 + 50) as i32;
        unsafe {
            *e.active.ptr() = 1;
            *e.id.ptr() = (j + 1) as i32;
            *e.num_used.ptr() = 0;
            *e.num_free.ptr() = num;
            *e.num_total.ptr() = num;
            *e.price.ptr() = p_val;
        }
        e
    }).collect();
    let flights: Vec<ResEntry> = (0..num_relations).map(|j| {
        let e = ResEntry::new();
        let num = ((init_rng.next() % 5) + 1) as i32 * 100;
        let p_val = ((init_rng.next() % 5) * 10 + 50) as i32;
        unsafe {
            *e.active.ptr() = 1;
            *e.id.ptr() = (j + 1) as i32;
            *e.num_used.ptr() = 0;
            *e.num_free.ptr() = num;
            *e.num_total.ptr() = num;
            *e.price.ptr() = p_val;
        }
        e
    }).collect();
    let customers: Vec<CustEntry> = (0..num_relations).map(|j| {
        let e = CustEntry::new();
        unsafe { *e.id.ptr() = (j + 1) as i32; }
        e
    }).collect();

    let tables = Arc::new((cars, rooms, flights, customers));

    let total_tasks = config.duration.max(1); // use duration as task count per thread
    let dur = std::time::Duration::from_millis(config.duration as u64);

    let g_ops_counter = AtomicU64::new(0);

    let t0 = std::time::Instant::now();
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let t = tables.clone();
            let sc = stop;
            let go = &g_ops_counter;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                for _ in 0..total_tasks {
                    if sc.load(Ordering::Relaxed) { break; }
                    let customer_id = ((rng.borrow_mut().next() % query_range as u64) + 1) as usize;
                    let choice = rng.borrow_mut().next() % 100;

                    if choice < percent_user as u64 {
                        // Make reservation (best-price)
                        transaction(|tx| {
                            // Add customer if not exists
                            let c = &t.3[customer_id - 1];
                            if tx.read(&c.active) == 0 {
                                tx.write(&c.active, 1);
                                tx.write(&c.id, customer_id as i32);
                                tx.write(&c.bill, 0);
                            }

                            // Multiple queries to find best prices
                            let tables_arr: [&[ResEntry]; 3] = [&t.0, &t.1, &t.2];
                            let mut best_prices = [-1i32; 3];
                            let mut best_ids = [0usize; 3];
                            let mut found = false;
                            let nq = (rng.borrow_mut().next() % num_queries_per_tx as u64) + 1;
                            for _ in 0..nq {
                                let tt = (rng.borrow_mut().next() % 3) as usize;
                                let id = ((rng.borrow_mut().next() % query_range as u64) + 1) as usize;
                                let e = &tables_arr[tt][id - 1];
                                if tx.read(&e.active) != 0 {
                                    let avail = tx.read(&e.num_free);
                                    if avail > 0 {
                                        let price = tx.read(&e.price);
                                        if price > best_prices[tt] {
                                            best_prices[tt] = price;
                                            best_ids[tt] = id;
                                            found = true;
                                        }
                                    }
                                }
                            }
                            if found {
                                for tt in 0..3 {
                                    if best_ids[tt] > 0 {
                                        let e = &tables_arr[tt][best_ids[tt] - 1];
                                        let used = tx.read(&e.num_used);
                                        let free = tx.read(&e.num_free);
                                        tx.write(&e.num_used, used + 1);
                                        tx.write(&e.num_free, free - 1);
                                        let price = tx.read(&e.price);
                                        let bill = tx.read(&c.bill);
                                        tx.write(&c.bill, bill + price);
                                    }
                                }
                            }
                        });
                    } else if choice % 2 == 0 {
                        // Delete customer
                        transaction(|tx| {
                            let c = &t.3[customer_id - 1];
                            if tx.read(&c.active) != 0 {
                                tx.write(&c.active, 0);
                            }
                        });
                    } else {
                        // Update tables (add/delete reservation)
                        transaction(|tx| {
                            let tt = (rng.borrow_mut().next() % 3) as usize;
                            let id = ((rng.borrow_mut().next() % query_range as u64) + 1) as usize;
                            let op = rng.borrow_mut().next() % 2;
                            let tables_arr: [&[ResEntry]; 3] = [&t.0, &t.1, &t.2];
                            let e = &tables_arr[tt][id - 1];
                            if op == 1 {
                                let price = ((rng.borrow_mut().next() % 5) * 10 + 50) as i32;
                                if tx.read(&e.active) != 0 {
                                    tx.write(&e.num_free, tx.read(&e.num_free) + 100);
                                    tx.write(&e.num_total, tx.read(&e.num_total) + 100);
                                    tx.write(&e.price, price);
                                } else {
                                    tx.write(&e.active, 1);
                                    tx.write(&e.id, id as i32);
                                    tx.write(&e.num_used, 0);
                                    tx.write(&e.num_free, 100);
                                    tx.write(&e.num_total, 100);
                                    tx.write(&e.price, price);
                                }
                            } else {
                                if tx.read(&e.active) != 0 {
                                    let free = tx.read(&e.num_free);
                                    if free >= 100 {
                                        tx.write(&e.num_free, free - 100);
                                        tx.write(&e.num_total, tx.read(&e.num_total) - 100);
                                        if tx.read(&e.num_total) == 0 {
                                            tx.write(&e.active, 0);
                                        }
                                    }
                                }
                            }
                        });
                    }
                    go.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;
    let ops_count = g_ops_counter.load(Ordering::Relaxed);
    println!("  Operations: {}  Elapsed: {} ms  Rate: {} ops/s",
             ops_count, elapsed, if elapsed > 0 { ops_count * 1000 / elapsed } else { 0 });
}
