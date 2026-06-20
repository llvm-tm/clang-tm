use std::sync::Arc;
use tm::{TmCell, transaction, tm_init, tm_exit, tm_abort_count};
use tm_executor::{QueueExecutor, tx_execute};

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
    if args.len() == 2 && (args[1] == "--version" || args[1] == "-V") {
        println!("\n\
━┏━━━┓┏┓━┏┓┏━━━━┓┏━━━━┓\n\
━┃┏━━┛┃┃━┃┃┗━━┓━┃┗━━┓━┃\n\
━┃┗━━┓┃┃━┃┃━━┏┛┏┛━━┏┛┏┛\n\
━┃┏━━┛┃┃━┃┃━┏┛┏┛━━┏┛┏┛━\n\
┏┛┗┓━━┃┗━┛┃┏┛━┗━┓┏┛━┗━┓\n\
┗━━┛━━┗━━━┛┗━━━━┛┗━━━━┛\n\
    fuzz-bank v1.0.0\n");
        return;
    }

    let mut threads = 4usize;
    let mut iters = 1000usize;
    let mut accounts = 64usize;
    let mut seed = 42u64;
    let mut queue_mode = false;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1 < args.len() => { threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-n" if i+1 < args.len() => { iters = args[i+1].parse().unwrap_or(1000); i+=2; }
            "-a" if i+1 < args.len() => { accounts = args[i+1].parse().unwrap_or(64); i+=2; }
            "-s" if i+1 < args.len() => { seed = args[i+1].parse().unwrap_or(42); i+=2; }
            "--queue" => { queue_mode = true; i+=1; }
            "-h" | "--help" => {
                eprintln!("Usage: fuzz_bank [-t threads] [-n iters] [-a accounts] [-s seed] [--queue]");
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

    let exec: Option<Arc<QueueExecutor>> = if queue_mode {
        println!("Mode: queue ({} workers, {} queues)", threads, threads);
        Some(Arc::new(QueueExecutor::new(threads, threads)))
    } else {
        println!("Mode: inline");
        None
    };

    let mut handles = Vec::with_capacity(threads);
    for tid in 0..threads {
        let bank = Arc::clone(&bank);
        let exec = exec.clone();
        handles.push(std::thread::spawn(move || {
            let mut rng = fastrand::Rng::new();
            rng.seed(seed.wrapping_add(tid as u64 * 12345));
            for _ in 0..iters {
                let src = rng.usize(0..accounts);
                let mut dst = rng.usize(0..accounts);
                if dst == src { dst = (src + 1) % accounts; }
                let amount = rng.i32(1..=100);
                let b = Arc::clone(&bank);
                let f = move |tx: &tm::Transaction| {
                    let sb = tx.read(&b[src]);
                    if sb >= amount {
                        tx.write(&b[src], sb - amount);
                        let db = tx.read(&b[dst]);
                        tx.write(&b[dst], db + amount);
                    }
                };
                match &exec {
                    Some(e) => tx_execute(e.as_ref(), f),
                    None => transaction(f),
                }
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
