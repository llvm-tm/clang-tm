use std::sync::Arc;
use tm::{TmCell, transaction, tm_init, tm_exit, tm_abort_count};

fn verify(bank: &[TmCell<i32>], expected: i32) {
    let total: i32 = bank.iter().map(|a| unsafe { *a.ptr() }).sum();
    if total != expected {
        eprintln!("FAIL: Money {} by {} (got {}, expected {})",
                  if total > expected { "created" } else { "destroyed" },
                  if total > expected { total - expected } else { expected - total },
                  total, expected);
        tm_exit();
        std::process::exit(1);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut threads = 4usize;
    let mut iters = 1000usize;
    let mut accounts = 16usize;
    let mut seed = 42u64;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1 < args.len() => { threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-n" if i+1 < args.len() => { iters = args[i+1].parse().unwrap_or(1000); i+=2; }
            "-a" if i+1 < args.len() => { accounts = args[i+1].parse().unwrap_or(16); i+=2; }
            "-s" if i+1 < args.len() => { seed = args[i+1].parse().unwrap_or(42); i+=2; }
            "-h" | "--help" => {
                eprintln!("Usage: fuzz_bank [-t threads] [-n iters] [-a accounts] [-s seed]");
                return;
            }
            _ => i+=1,
        }
    }

    println!("Fuzz Bank — Rust TM API");
    println!("Threads: {threads}  Iters: {iters}  Accounts: {accounts}  Seed: {seed}");

    tm_init();

    let init_balance = 10000i32;
    let bank: Arc<Vec<TmCell<i32>>> = Arc::new(
        (0..accounts).map(|_| TmCell::new(init_balance)).collect()
    );

    let expected_initial = (accounts as i32) * init_balance;
    verify(&bank, expected_initial);

    let mut handles = Vec::with_capacity(threads);
    for tid in 0..threads {
        let bank = Arc::clone(&bank);
        handles.push(std::thread::spawn(move || {
            let mut rng = fastrand::Rng::new();
            rng.seed(seed.wrapping_add(tid as u64 * 12345));
            for _ in 0..iters {
                let src = rng.usize(0..accounts);
                let mut dst = rng.usize(0..accounts);
                if dst == src { dst = (src + 1) % accounts; }
                let amount = rng.i32(1..=100);
                transaction(|tx| {
                    let sb = tx.read(&bank[src]);
                    if sb >= amount {
                        tx.write(&bank[src], sb - amount);
                        let db = tx.read(&bank[dst]);
                        tx.write(&bank[dst], db + amount);
                    }
                });
            }
        }));
    }

    for h in handles {
        h.join().unwrap();
    }

    verify(&bank, expected_initial);
    println!("\nResults:");
    println!("  TM aborts: {}", tm_abort_count());
    println!("PASS: Money conserved");

    tm_exit();
}
