// ── YCSB: Yahoo! Cloud Serving Benchmark (Rust/TM port) ────────────────
//
// Workloads A-F, Zipfian/Uniform/Latest distribution, configurable
// key range, threads, duration.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;

use tm::{transaction, TmCell, TmPtr, Transaction};

// ── Config ─────────────────────────────────────────────────────────────

#[derive(Clone, Copy, PartialEq)]
enum Workload { A, B, C, D, E, F }

#[derive(Clone, Copy, PartialEq)]
enum Distribution { Uniform, Zipfian, Latest }

#[derive(Clone)]
struct Config {
    threads: usize,
    duration: usize,
    workload: Workload,
    dist: Distribution,
    key_range: usize,
    initial_records: usize,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            threads: 4, duration: 10000,
            workload: Workload::A,
            dist: Distribution::Zipfian,
            key_range: 10000,
            initial_records: 10000,
        }
    }
}

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1<args.len() => { c.threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-d" if i+1<args.len() => { c.duration = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-w" if i+1<args.len() => {
                c.workload = match args[i+1].to_lowercase().as_str() {
                    "a" => Workload::A, "b" => Workload::B, "c" => Workload::C,
                    "d" => Workload::D, "e" => Workload::E, "f" => Workload::F,
                    _ => Workload::A,
                };
                i+=2;
            }
            "-k" if i+1<args.len() => { c.key_range = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-i" if i+1<args.len() => { c.initial_records = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-dist" if i+1<args.len() => {
                c.dist = match args[i+1].to_lowercase().as_str() {
                    "u"|"uniform" => Distribution::Uniform,
                    "z"|"zipfian" => Distribution::Zipfian,
                    "l"|"latest" => Distribution::Latest,
                    _ => Distribution::Zipfian,
                };
                i+=2;
            }
            _ => i+=1,
        }
    }
    c
}

// ── Zipfian distribution ──────────────────────────────────────────────

fn zeta(n: usize, theta: f64) -> f64 {
    (0..n).map(|i| 1.0 / (i as f64 + 1.0).powf(theta)).sum()
}

fn build_zipfian_cdf(n: usize, theta: f64) -> Vec<f64> {
    let z = zeta(n, theta);
    let mut cdf = Vec::with_capacity(n);
    let mut sum = 0.0;
    for i in 0..n {
        sum += 1.0 / (i as f64 + 1.0).powf(theta);
        cdf.push(sum / z);
    }
    cdf
}

fn zipfian_sample(cdf: &[f64], r: f64) -> usize {
    for (i, &v) in cdf.iter().enumerate() {
        if r < v { return i; }
    }
    cdf.len() - 1
}

// ── Record ────────────────────────────────────────────────────────────

const NUM_FIELDS: usize = 10;
const FIELD_SIZE: usize = 100;

struct Record {
    key: i64,
    data: [TmCell<u8>; NUM_FIELDS * FIELD_SIZE],
    timestamp: TmCell<i64>,
}

impl Record {
    fn new(key: i64) -> Self {
        let mut data: [TmCell<u8>; NUM_FIELDS * FIELD_SIZE] = unsafe { std::mem::zeroed() };
        for (i, cell) in data.iter_mut().enumerate() {
            let ch = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                [(key as usize + i) % 62];
            unsafe { *cell.ptr() = ch; }
        }
        Record { key, data, timestamp: TmCell::new(key) }
    }

    fn read_field0(&self, tx: &Transaction) -> [u8; FIELD_SIZE] {
        let mut out = [0u8; FIELD_SIZE];
        for i in 0..FIELD_SIZE {
            out[i] = tx.read(&self.data[i]);
        }
        out
    }

    fn update(&self, tx: &Transaction, key: i64) {
        for (i, cell) in self.data.iter().enumerate() {
            let ch = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                [(key as usize + i) % 62];
            tx.write(cell, ch);
        }
        tx.write(&self.timestamp, key);
    }
}

// ── Database ──────────────────────────────────────────────────────────

struct Database {
    records: Vec<TmCell<TmPtr<Record>>>,
    key_to_idx: Vec<TmCell<i64>>,  // key hash -> index (-1 = empty)
    count: AtomicU64,
    max_records: usize,
}

impl Database {
    fn new(max_records: usize) -> Self {
        let records = (0..max_records).map(|_| TmCell::new(TmPtr::null())).collect();
        let key_to_idx = (0..max_records).map(|_| TmCell::new(-1i64)).collect();
        Database { records, key_to_idx, count: AtomicU64::new(0), max_records }
    }

    fn hash(&self, key: i64) -> usize {
        let h = key as usize;
        (h.wrapping_mul(0x9e3779b9) >> 22) % self.max_records
    }

    fn insert(&self, tx: &Transaction, key: i64) {
        if self.count.load(Ordering::Relaxed) as usize >= self.max_records { return; }
        let h = self.hash(key);
        for offset in 0..self.max_records {
            let idx = (h + offset) % self.max_records;
            let stored = tx.read(&self.key_to_idx[idx]);
            if stored == -1 {
                let rec = Box::into_raw(Box::new(Record::new(key)));
                tx.write(&self.key_to_idx[idx], key);
                tx.write(&self.records[idx], TmPtr::new(rec));
                self.count.fetch_add(1, Ordering::Relaxed);
                return;
            }
            if stored == key { return; }
        }
    }

    fn find(&self, tx: &Transaction, key: i64) -> Option<*mut Record> {
        let h = self.hash(key);
        for offset in 0..self.max_records {
            let idx = (h + offset) % self.max_records;
            let stored = tx.read(&self.key_to_idx[idx]);
            if stored == -1 { return None; }
            if stored == key {
                let ptr = tx.read(&self.records[idx]);
                return Some(ptr.get());
            }
        }
        None
    }

    fn update(&self, tx: &Transaction, key: i64) {
        if let Some(rec) = self.find(tx, key) {
            let rec = unsafe { &*rec };
            rec.update(tx, key);
        }
    }

    fn read_field0(&self, tx: &Transaction, key: i64) -> Option<[u8; FIELD_SIZE]> {
        self.find(tx, key).map(|rec| unsafe { (*rec).read_field0(tx) })
    }

    fn scan(&self, tx: &Transaction, start_key: i64, count: usize) -> Vec<i64> {
        let mut results = Vec::new();
        for i in 0..count.min(10) {
            let k = start_key + i as i64;
            if let Some(rec) = self.find(tx, k) {
                results.push(unsafe { (*rec).key });
            }
        }
        results
    }
}

// ── Worker ─────────────────────────────────────────────────────────────

fn run_worker(
    tid: usize,
    db: &Database,
    config: &Config,
    cdf: &[f64],
    stop: &AtomicBool,
    ops: &Arc<AtomicU64>,
) {
    let mut rng = Rng::new(tid as u64 * 12345 + 42);
    let mut insert_counter = config.key_range as i64 + tid as i64;

    while !stop.load(Ordering::Relaxed) {
        let r: f64 = (rng.next() as f64) / (u64::MAX as f64);
        let key = match config.dist {
            Distribution::Zipfian => {
                let sample = zipfian_sample(cdf, r);
                (sample as i64) % config.key_range as i64
            }
            Distribution::Uniform => {
                (rng.next() % config.key_range as u64) as i64
            }
            Distribution::Latest => {
                let max = (config.key_range as i64 + insert_counter as i64).min(config.key_range as i64 * 2);
                max - 1 - (rng.next() % 1000u64) as i64
            }
        };

        let op_r = (r * 100.0) as usize;

        match config.workload {
            Workload::A => {
                if op_r < 50 {
                    let _ = transaction(|tx| db.read_field0(tx, key));
                } else {
                    transaction(|tx| db.update(tx, key));
                }
            }
            Workload::B => {
                if op_r < 95 {
                    let _ = transaction(|tx| db.read_field0(tx, key));
                } else {
                    transaction(|tx| db.update(tx, key));
                }
            }
            Workload::C => {
                let _ = transaction(|tx| db.read_field0(tx, key));
            }
            Workload::D => {
                if op_r < 95 {
                    let _ = transaction(|tx| db.read_field0(tx, key));
                } else {
                    transaction(|tx| db.insert(tx, insert_counter));
                    insert_counter += config.threads as i64;
                }
            }
            Workload::E => {
                if op_r < 95 {
                    let _ = transaction(|tx| db.read_field0(tx, key));
                } else {
                    let _ = transaction(|tx| db.scan(tx, key, 10));
                }
            }
            Workload::F => {
                if op_r < 50 {
                    let _ = transaction(|tx| db.scan(tx, key, 10));
                } else {
                    let _ = transaction(|tx| db.read_field0(tx, key));
                    transaction(|tx| db.update(tx, key));
                }
            }
        }
        ops.fetch_add(1, Ordering::Relaxed);
    }
}

struct Rng(u64);
impl Rng {
    fn new(seed: u64) -> Self { Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) }
    fn next(&mut self) -> u64 { self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); self.0 >> 33 }
}

// ── Main ───────────────────────────────────────────────────────────────

fn main() {
    tm::tm_init();
    let config = parse_args();

    let workload_names = ["A", "B", "C", "D", "E", "F"];
    let dist_names = ["uniform", "zipfian", "latest"];

    println!("========= YCSB Benchmark =========");
    println!("===================================");
    println!("Workload:     {}", workload_names[config.workload as usize]);
    println!("Distribution: {}", dist_names[config.dist as usize]);
    println!("Threads:      {}", config.threads);
    println!("Duration:     {} ms", config.duration);
    println!("Key range:    {}", config.key_range);
    println!("Initial recs: {}", config.initial_records);
    println!();

    let cdf = if config.dist == Distribution::Zipfian {
        build_zipfian_cdf(10000, 0.99)
    } else {
        Vec::new()
    };

    let db = Arc::new(Database::new(100_000));

    // Load initial records
    println!("Loading {} records...", config.initial_records);
    for key in 0..config.initial_records as i64 {
        transaction(|tx| db.insert(tx, key));
    }
    println!("  Loaded: {}\n", db.count.load(Ordering::Relaxed));

    let stop = Arc::new(AtomicBool::new(false));
    let total_ops = Arc::new(AtomicU64::new(0));
    let duration_ms = config.duration;

    let mut handles = Vec::new();
    for tid in 0..config.threads {
        let stop = stop.clone();
        let ops = total_ops.clone();
        let db = db.clone();
        let cdf = cdf.clone();
        let cfg = config.clone();
        handles.push(std::thread::spawn(move || {
            run_worker(tid, &db, &cfg, &cdf, &stop, &ops);
        }));
    }

    std::thread::sleep(std::time::Duration::from_millis(duration_ms as u64));
    stop.store(true, Ordering::Relaxed);
    for h in handles { h.join().unwrap(); }

    let ops = total_ops.load(Ordering::Relaxed);
    let elapsed = duration_ms as f64 / 1000.0;

    println!("\nResults");
    println!("=======");
    println!("Elapsed:  {:.1}s", elapsed);
    println!("Total ops: {ops}");
    println!("Throughput: {:.0} ops/s", ops as f64 / elapsed);

    tm::tm_exit();
}
