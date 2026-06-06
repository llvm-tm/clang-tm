use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tm::{transaction, TmCell};

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1<args.len() => { c.threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-d" if i+1<args.len() => { c.duration = args[i+1].parse().unwrap_or(10000); i+=2; }
            "--r1" if i+1<args.len() => { c.r1 = args[i+1].parse().unwrap_or(10); i+=2; }
            "--w1" if i+1<args.len() => { c.w1 = args[i+1].parse().unwrap_or(10); i+=2; }
            "--r2" if i+1<args.len() => { c.r2 = args[i+1].parse().unwrap_or(10); i+=2; }
            "--w2" if i+1<args.len() => { c.w2 = args[i+1].parse().unwrap_or(10); i+=2; }
            "--a1" if i+1<args.len() => { c.a1 = args[i+1].parse().unwrap_or(100); i+=2; }
            "--a2" if i+1<args.len() => { c.a2 = args[i+1].parse().unwrap_or(10000); i+=2; }
            "--a3" if i+1<args.len() => { c.a3 = args[i+1].parse().unwrap_or(10000); i+=2; }
            "--contention" if i+1<args.len() => { c.contention = args[i+1].parse().unwrap_or(0.5); i+=2; }
            "--locality" if i+1<args.len() => { c.locality = args[i+1].parse().unwrap_or(0.5); i+=2; }
            "--density" if i+1<args.len() => { c.density = args[i+1].parse().unwrap_or(0.5); i+=2; }
            "--mode" if i+1<args.len() => { c.mode = args[i+1].parse().unwrap_or(0); i+=2; }
            "--enable-r2" => { c.r2_enabled = true; i+=1; }
            "--enable-r3" => { c.r3_enabled = true; i+=1; }
            _ => i+=1,
        }
    }
    c
}

#[derive(Clone)]
struct Config {
    threads: usize,
    duration: usize,
    r1: usize, w1: usize,
    r2: usize, w2: usize,
    a1: usize, a2: usize, a3: usize,
    contention: f64,
    locality: f64,
    density: f64,
    r2_enabled: bool,
    r3_enabled: bool,
    mode: usize,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            threads: 4, duration: 10000,
            r1: 10, w1: 10, r2: 10, w2: 10,
            a1: 100, a2: 10000, a3: 10000,
            contention: 0.5, locality: 0.5, density: 0.5,
            r2_enabled: true, r3_enabled: false, mode: 0,
        }
    }
}

struct SharedArrays {
    a1: Vec<TmCell<i64>>,
    a2: Vec<Vec<TmCell<i64>>>,
    a3: Vec<Vec<TmCell<i64>>>,
    #[allow(dead_code)]
    config: Config,
}

impl SharedArrays {
    fn new(config: &Config) -> Self {
        let a1: Vec<_> = (0..config.a1).map(|_| TmCell::new(0)).collect();
        let a2 = (0..config.threads).map(|_| (0..config.a2).map(|_| TmCell::new(0)).collect()).collect();
        let a3 = (0..config.threads).map(|_| (0..config.a3).map(|_| TmCell::new(0)).collect()).collect();
        SharedArrays { a1, a2, a3, config: config.clone() }
    }
}

fn run_worker(
    tid: usize,
    arrays: &SharedArrays,
    config: &Config,
    stop: &AtomicBool,
    ops: &Arc<AtomicU64>,
) {
    let r1_idxs: Vec<usize> = (0..config.r1).map(|_| {
        if config.contention > 0.5 { tid % config.a1 } else {
            (tid.wrapping_mul(137) + tid.wrapping_mul(tid)) % config.a1
        }
    }).collect();
    let w1_idxs: Vec<usize> = (0..config.w1).map(|_| {
        if config.contention > 0.5 { tid % config.a1 } else {
            (tid.wrapping_mul(173) + tid.wrapping_mul(tid)) % config.a1
        }
    }).collect();

    while !stop.load(Ordering::Relaxed) {
        transaction(|tx| {
            let mut sum: i64 = 0;

            for &idx in &r1_idxs {
                sum = sum.wrapping_add(tx.read(&arrays.a1[idx % config.a1]));
            }

            for (j, &idx) in w1_idxs.iter().enumerate() {
                tx.write(&arrays.a1[idx % config.a1], sum + j as i64);
            }

            if config.r2_enabled {
                let base = tid * config.a2;
                for j in 0..config.r2 {
                    let idx = if config.locality > 0.8 { base + j % config.a2 }
                              else if config.locality > 0.5 { base + r1_idxs[j % r1_idxs.len()] % config.a2 }
                              else { base + (j * 137) % config.a2 };
                    sum = sum.wrapping_add(tx.read(&arrays.a2[tid][idx % config.a2]));
                }
                for j in 0..config.w2 {
                    let idx = if config.density > 0.8 { base + j }
                              else if config.density > 0.5 { base + w1_idxs[j % w1_idxs.len()] % config.a2 }
                              else { base + (j * 173) % config.a2 };
                    tx.write(&arrays.a2[tid][idx % config.a2], sum);
                }
            }
        });
        ops.fetch_add(1, Ordering::Relaxed);

        if config.r3_enabled && config.mode != 1 {
            for j in 0..config.a3.min(100) {
                let idx = if config.locality > 0.8 { j % config.a3 }
                          else { (j * 137) % config.a3 };
                let cell = &arrays.a3[tid][idx];
                unsafe {
                    let v = *cell.ptr();
                    *cell.ptr() = v + 1;
                }
            }
        }
    }
}

fn main() {
    let config = parse_args();

    println!("========= EigenBench =========");
    println!("===============================");
    println!("Threads: {t}  Duration: {d}ms", t=config.threads, d=config.duration);
    println!("R1: {r1}  W1: {w1}  R2: {r2}  W2: {w2}", r1=config.r1, w1=config.w1, r2=config.r2, w2=config.w2);
    println!("A1: {a1}  A2: {a2}  A3: {a3}", a1=config.a1, a2=config.a2, a3=config.a3);
    println!("Contention: {:.2}  Locality: {:.2}  Density: {:.2}", config.contention, config.locality, config.density);
    println!("R2 enabled: {}  R3 enabled: {}  Mode: {}", config.r2_enabled, config.r3_enabled, config.mode);
    println!();

    tm::tm_init();

    let arrays = Arc::new(SharedArrays::new(&config));
    let stop = Arc::new(AtomicBool::new(false));
    let total_ops = Arc::new(AtomicU64::new(0));

    let mut handles = Vec::new();
    for tid in 0..config.threads {
        let stop = stop.clone();
        let ops = total_ops.clone();
        let arr = arrays.clone();
        let cfg = config.clone();
        handles.push(std::thread::spawn(move || {
            run_worker(tid, &arr, &cfg, &stop, &ops);
        }));
    }

    std::thread::sleep(std::time::Duration::from_millis(config.duration as u64));
    stop.store(true, Ordering::Relaxed);
    for h in handles { h.join().unwrap(); }

    let ops = total_ops.load(Ordering::Relaxed);
    let secs = config.duration as f64 / 1000.0;

    println!("\nResults");
    println!("=======");
    println!("Total TXs: {ops}");
    println!("Throughput: {:.0} tx/s", ops as f64 / secs);
    println!("TM aborts: {}", tm::tm_abort_count());

    tm::tm_exit();
}
