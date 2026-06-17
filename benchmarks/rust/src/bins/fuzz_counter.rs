use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use tm::{TmCell, transaction, tm_init, tm_exit, tm_abort_count};

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
    fuzz-counter v1.0.0\n");
        return;
    }

    let mut threads = 4usize;
    let mut iters = 1000usize;
    let mut counters = 8usize;
    let mut seed = 42u64;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1 < args.len() => { threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-n" if i+1 < args.len() => { iters = args[i+1].parse().unwrap_or(1000); i+=2; }
            "-c" if i+1 < args.len() => { counters = args[i+1].parse().unwrap_or(8); i+=2; }
            "-s" if i+1 < args.len() => { seed = args[i+1].parse().unwrap_or(42); i+=2; }
            "-h" | "--help" => {
                eprintln!("Usage: fuzz_counter [-t threads] [-n iters] [-c counters] [-s seed]");
                return;
            }
            _ => i+=1,
        }
    }

    println!("Fuzz Counter — Rust TM API");
    println!("Threads: {threads}  Iters: {iters}  Counters: {counters}  Seed: {seed}");

    tm_init();

    let vals: Arc<Vec<TmCell<u64>>> = Arc::new((0..counters).map(|_| TmCell::new(1000)).collect());
    let total_delta = Arc::new(AtomicU64::new(0));

    let mut handles = Vec::with_capacity(threads);
    for tid in 0..threads {
        let vals = Arc::clone(&vals);
        let total_delta = Arc::clone(&total_delta);
        handles.push(std::thread::spawn(move || {
            let mut rng = fastrand::Rng::new();
            rng.seed(seed.wrapping_add(tid as u64 * 12345));
            let mut local_delta = 0u64;
            for _ in 0..iters {
                let idx = rng.usize(0..counters);
                let inc = rng.u64(1..=10);
                transaction(|tx| {
                    let old = tx.read(&vals[idx]);
                    tx.write(&vals[idx], old + inc);
                });
                local_delta += inc;
            }
            total_delta.fetch_add(local_delta, Ordering::Relaxed);
        }));
    }

    for h in handles {
        h.join().unwrap();
    }

    let initial_sum = counters as u64 * 1000;
    let final_sum: u64 = vals.iter().map(|v| unsafe { *v.ptr() }).sum();
    let expected = initial_sum + total_delta.load(Ordering::Relaxed);

    println!("\nResults:");
    println!("  Initial sum: {initial_sum}");
    println!("  Total deltas: {expected} - {initial_sum} = {}", expected - initial_sum);
    println!("  Final sum:   {final_sum}");
    println!("  Expected:    {expected}");
    println!("  TM aborts:   {}", tm_abort_count());

    if final_sum == expected {
        println!("PASS: Counter invariant holds");
        tm_exit();
        return;
    }

    eprintln!("FAIL: Counter invariant violated");
    eprintln!("  Got {final_sum}, expected {expected}, diff = {}({})",
             if final_sum > expected { final_sum - expected } else { expected - final_sum },
             if final_sum > expected { "created" } else { "lost" });
    tm_exit();
    std::process::exit(1);
}
