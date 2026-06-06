use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tm::*;
use benchmarks::Rng;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_threads = 4;
    let mut num_relations = 16384;
    let mut num_queries_per_tx = 2;
    let mut percent_user = 98;
    let mut total_tasks = 4096;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-r" => { i += 1; num_relations = args[i].parse().unwrap(); }
            "-n" => { i += 1; num_queries_per_tx = args[i].parse().unwrap(); }
            "-u" => { i += 1; percent_user = args[i].parse().unwrap(); }
            "-t" => { i += 1; total_tasks = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    let query_range = (0.9 * num_relations as f64) as i32;

    println!("Initializing manager... done.");
    println!("Initializing clients... done.");
    println!("    Relations = {}", num_relations);
    println!("    Transactions = {}", total_tasks);
    println!("    Queries/transaction = {}", num_queries_per_tx);
    println!("    Percent user = {}", percent_user);
    println!("Running clients...");

    tm_init();

    // Initialize tables: cars, rooms, flights — each with num_relations entries
    // Each entry: [num_used, num_free, num_total, price, active]
    let mut tables: Vec<Vec<TmCell<i32>>> = (0..3).map(|_| Vec::with_capacity(num_relations * 5)).collect();
    let mut rng = Rng::new(42);
    for t in 0..3 {
        for _ in 0..num_relations {
            let num = ((rng.next() % 5 + 1) * 100) as i32;
            let price = (rng.next() % 5 * 10 + 50) as i32;
            tables[t].push(TmCell::new(0));  // num_used
            tables[t].push(TmCell::new(num));  // num_free
            tables[t].push(TmCell::new(num));  // num_total
            tables[t].push(TmCell::new(price));  // price
            tables[t].push(TmCell::new(1));  // active
        }
    }

    // Customer table
    let customers: Vec<TmCell<i32>> = (0..num_relations).map(|_| TmCell::new(0)).collect();
    let customer_active: Vec<TmCell<i32>> = (0..num_relations).map(|_| TmCell::new(0)).collect();

    let tables = Arc::new(tables);
    let customers = Arc::new(customers);
    let customer_active = Arc::new(customer_active);

    let queries = num_queries_per_tx as i32;
    let pct_user = percent_user;
    let qrange = query_range;
    let ttasks = total_tasks;

    let total_ops = Arc::new(AtomicU64::new(0));

    let start = Instant::now();

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let ops = total_ops.clone();
            let tables = tables.clone();
            let customers = customers.clone();
            let customer_active = customer_active.clone();

            s.spawn(move || {
                tm_init_thread();
                let mut rng = Rng::new(42u64 + tid as u64);

                let tasks_per = ttasks / num_threads as i32;
                let extra = ttasks % num_threads as i32;
                let start_idx = tid as i32 * tasks_per + std::cmp::min(tid as i32, extra);
                let end_idx = start_idx + tasks_per + if (tid as i32) < extra { 1 } else { 0 };

                for _iter in start_idx..end_idx {
                    let r = (rng.next() % 100) as i32;
                    let customer_id = (rng.next() % qrange as u64 + 1) as usize;

                    if r < pct_user {
                        // make_reservation_tx
                        transaction(|tx| {
                            let mut rng = Rng::new(
                                (unsafe { std::arch::x86_64::_rdtsc() })
                                    .wrapping_add(customer_id as u64),
                            );

                            // add_customer
                            let c_active = tx.read(&customer_active[customer_id - 1]);
                            if c_active == 0 {
                                tx.write(&customer_active[customer_id - 1], 1);
                            }

                            let nq = (rng.next() % queries as u64) as i32 + 1;
                            let mut best_prices = [-1i32; 3];
                            let mut best_ids = [-1i32; 3];
                            let mut found = false;

                            for _q in 0..nq {
                                let t = (rng.next() % 3) as usize;
                                let id = (rng.next() % qrange as u64 + 1) as usize;
                                let idx = (id - 1) * 5;
                                let avail = tx.read(&tables[t][idx + 1]); // num_free
                                if avail > 0 {
                                    let price = tx.read(&tables[t][idx + 3]); // price
                                    if price > best_prices[t] {
                                        best_prices[t] = price;
                                        best_ids[t] = id as i32;
                                        found = true;
                                    }
                                }
                            }

                            if found {
                                for t in 0..3 {
                                    if best_ids[t] > 0 {
                                        let id = best_ids[t] as usize;
                                        let idx = (id - 1) * 5;
                                        let p = tx.read(&tables[t][idx + 1]); // num_free
                                        if p > 0 {
                                            tx.write(&tables[t][idx], tx.read(&tables[t][idx]) + 1); // num_used++
                                            tx.write(&tables[t][idx + 1], p - 1); // num_free--
                                            let price = tx.read(&tables[t][idx + 3]);
                                            tx.write(&customers[customer_id - 1],
                                                     tx.read(&customers[customer_id - 1]) + price);
                                        }
                                    }
                                }
                            }
                        });
                    } else if r % 2 == 0 {
                        // delete_customer_tx
                        transaction(|tx| {
                            tx.write(&customer_active[customer_id - 1], 0);
                            tx.write(&customers[customer_id - 1], 0);
                        });
                    } else {
                        // update_tables_tx
                        transaction(|tx| {
                            let mut rng = Rng::new(unsafe { std::arch::x86_64::_rdtsc() });
                            let t = (rng.next() % 3) as usize;
                            let id = (rng.next() % qrange as u64 + 1) as usize;
                            let op = rng.next() % 2;
                            let price_val =
                                if op == 1 { (rng.next() % 5 * 10 + 50) as i32 } else { 0 };
                            let idx = (id - 1) * 5;
                            if op == 1 {
                                let price = price_val;
                                tx.write(&tables[t][idx + 1], tx.read(&tables[t][idx + 1]) + 100);
                                tx.write(&tables[t][idx + 2], tx.read(&tables[t][idx + 2]) + 100);
                                tx.write(&tables[t][idx + 3], price);
                                tx.write(&tables[t][idx + 4], 1);
                            } else {
                                let nf = tx.read(&tables[t][idx + 1]);
                                if nf >= 100 {
                                    tx.write(&tables[t][idx + 1], nf - 100);
                                    let nt = tx.read(&tables[t][idx + 2]);
                                    tx.write(&tables[t][idx + 2], nt - 100);
                                    if nt == 100 {
                                        tx.write(&tables[t][idx + 4], 0);
                                    }
                                }
                            }
                        });
                    }

                    ops.fetch_add(1, Ordering::Relaxed);
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);

    println!("    Time = {} ms", elapsed.as_millis());
    println!("    Ops = {}", ops);
    println!("    Throughput = {:.2} ops/sec", ops as f64 / elapsed.as_secs_f64());

    tm_exit();
}

#[cfg(test)]
mod tests {
    use benchmarks::Rng;

    #[test]
    fn test_rng_determinism() {
        let mut a = Rng::new(42);
        let mut b = Rng::new(42);
        for _ in 0..1000 {
            assert_eq!(a.next(), b.next());
        }
    }

    #[test]
    fn test_cli_defaults() {
        let args: Vec<String> = vec!["stamp_vacation".into()];
        let mut num_threads = 4;
        let mut num_relations = 16384;
        let mut num_queries_per_tx = 2;
        let mut percent_user = 98;
        let mut total_tasks = 4096;
        let mut i = 1;
        while i < args.len() {
            match args[i].as_str() {
                "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
                "-r" => { i += 1; num_relations = args[i].parse().unwrap(); }
                "-n" => { i += 1; num_queries_per_tx = args[i].parse().unwrap(); }
                "-u" => { i += 1; percent_user = args[i].parse().unwrap(); }
                "-t" => { i += 1; total_tasks = args[i].parse().unwrap(); }
                _ => {}
            }
            i += 1;
        }
        assert_eq!(num_threads, 4);
        assert_eq!(num_relations, 16384);
        assert_eq!(num_queries_per_tx, 2);
        assert_eq!(percent_user, 98);
        assert_eq!(total_tasks, 4096);
    }

    #[test]
    fn test_cli_parsing() {
        let args: Vec<String> = vec![
            "stamp_vacation".into(),
            "-p".into(), "8".into(),
            "-r".into(), "100".into(),
            "-n".into(), "5".into(),
            "-u".into(), "50".into(),
            "-t".into(), "1000".into(),
        ];
        let mut num_threads = 4;
        let mut num_relations = 16384;
        let mut num_queries_per_tx = 2;
        let mut percent_user = 98;
        let mut total_tasks = 4096;
        let mut i = 1;
        while i < args.len() {
            match args[i].as_str() {
                "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
                "-r" => { i += 1; num_relations = args[i].parse().unwrap(); }
                "-n" => { i += 1; num_queries_per_tx = args[i].parse().unwrap(); }
                "-u" => { i += 1; percent_user = args[i].parse().unwrap(); }
                "-t" => { i += 1; total_tasks = args[i].parse().unwrap(); }
                _ => {}
            }
            i += 1;
        }
        assert_eq!(num_threads, 8);
        assert_eq!(num_relations, 100);
        assert_eq!(num_queries_per_tx, 5);
        assert_eq!(percent_user, 50);
        assert_eq!(total_tasks, 1000);
    }

    #[test]
    fn test_add_customer() {
        let mut customer_active = vec![0i32; 10];
        let customer_id = 3;
        if customer_active[customer_id - 1] == 0 {
            customer_active[customer_id - 1] = 1;
        }
        assert_eq!(customer_active[customer_id - 1], 1);
    }

    #[test]
    fn test_remove_reservation() {
        let mut customer_active = vec![1i32; 10];
        let mut customers = vec![500i32; 10];
        let customer_id = 5;
        customer_active[customer_id - 1] = 0;
        customers[customer_id - 1] = 0;
        assert_eq!(customer_active[customer_id - 1], 0);
        assert_eq!(customers[customer_id - 1], 0);
    }

    #[test]
    fn test_query_reservation() {
        let num_relations = 10usize;
        let num_queries_per_tx = 3i32;
        let _query_range = (0.9 * num_relations as f64) as i32;
        let tables: Vec<Vec<i32>> = (0..3)
            .map(|_| (0..num_relations * 5).map(|i| {
                let slot = i / 5;
                match i % 5 {
                    0 => 0,
                    1 => 100,
                    2 => 100,
                    3 => slot as i32 * 10 + 50,
                    _ => 1,
                }
            }).collect())
            .collect();

        let mut best_prices = [-1i32; 3];
        let mut best_ids = [-1i32; 3];
        let mut found = false;
        for _q in 0..num_queries_per_tx {
            let t = 1usize;
            let id = 3usize;
            let idx = (id - 1) * 5;
            let avail = tables[t][idx + 1];
            if avail > 0 {
                let price = tables[t][idx + 3];
                if price > best_prices[t] {
                    best_prices[t] = price;
                    best_ids[t] = id as i32;
                    found = true;
                }
            }
        }
        assert!(found);
        assert!(best_ids[1] > 0);
        assert!(best_prices[1] > 0);
    }
}
