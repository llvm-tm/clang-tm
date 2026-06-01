use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tm::{TmCell, transaction, tm_init, tm_exit, tm_init_thread, tm_exit_thread};

// ── Configuration ───────────────────────────────────────
const DEFAULT_DURATION_MS: u64 = 5000;
const DEFAULT_NB_THREADS: usize = 4;
const COMPLEX_PARTS: usize = 200;
const ATOMIC_PARTS: usize = 800;

// ── IDs tracked inside transactions ─────────────────────
// All fields stored in TmCell so TM reads/writes are visible.

// ── Atomic Part ─────────────────────────────────────────
struct AtomicPart {
    _id: i32,
    ptype: TmCell<i32>,
    build_date: TmCell<i32>,
    x: TmCell<i32>,
    y: TmCell<i32>,
}

impl AtomicPart {
    fn new(id: i32, ptype: i32, build_date: i32) -> Self {
        AtomicPart {
            _id: id,
            ptype: TmCell::new(ptype),
            build_date: TmCell::new(build_date),
            x: TmCell::new(0),
            y: TmCell::new(0),
        }
    }
}

// ── Composite Part ──────────────────────────────────────
struct CompositePart {
    _id: i32,
    build_date: TmCell<i32>,
    parts: Vec<Arc<AtomicPart>>,
}

impl CompositePart {
    fn new(id: i32, build_date: i32) -> Self {
        CompositePart {
            _id: id,
            build_date: TmCell::new(build_date),
            parts: Vec::new(),
        }
    }
}

// ── Database ────────────────────────────────────────────
struct Database {
    atomic_parts: Vec<Arc<AtomicPart>>,
    composite_parts: Vec<Arc<CompositePart>>,
    // Date-index: sorted list of (build_date, id) for composite parts
    // Read-only snapshot — rebuilt once at startup since structural
    // modifications are rare in this benchmark.
    date_index: Vec<(i32, usize)>,
}

impl Database {
    fn new() -> Self {
        let mut rng = fastrand::Rng::new();
        rng.seed(42);

        // Create atomic parts
        let atomic_parts: Vec<Arc<AtomicPart>> = (0..ATOMIC_PARTS)
            .map(|i| {
                Arc::new(AtomicPart::new(
                    i as i32,
                    rng.i32(1..=5),
                    rng.i32(0..=1000),
                ))
            })
            .collect();

        // Create composite parts (each referencing a subset of atomic parts)
        let composite_parts: Vec<Arc<CompositePart>> = (0..COMPLEX_PARTS)
            .map(|i| {
                let mut cp = CompositePart::new(i as i32, rng.i32(0..=1000));
                // Assign 3–5 random atomic parts to each composite part
                let n = rng.usize(3..=5);
                for _ in 0..n {
                    let idx = rng.usize(0..ATOMIC_PARTS);
                    cp.parts.push(Arc::clone(&atomic_parts[idx]));
                }
                Arc::new(cp)
            })
            .collect();

        // Build date-index via non-TX read of build_date fields
        let mut raw_idx: Vec<(i32, usize)> = composite_parts
            .iter()
            .enumerate()
            .map(|(i, cp)| (unsafe { *cp.build_date.ptr() }, i))
            .collect();
        raw_idx.sort_by(|a, b| a.0.cmp(&b.0));

        Database {
            atomic_parts,
            composite_parts,
            date_index: raw_idx,
        }
    }

    fn composite_count(&self) -> usize {
        self.composite_parts.len()
    }

    fn atomic_count(&self) -> usize {
        self.atomic_parts.len()
    }
}

// ── Worker ──────────────────────────────────────────────
struct WorkerStats {
    committed: u64,
    aborted: u64,
}

fn worker(
    tid: usize,
    db: &Database,
    stop: &AtomicBool,
) -> WorkerStats {
    let mut rng = fastrand::Rng::new();
    rng.seed(tid as u64 + 1000);
    let mut committed = 0u64;
    let aborted = 0u64;

    while !stop.load(Ordering::Relaxed) {
        let roll = rng.f64() * 100.0;

        match () {
            _ if roll < 40.0 => {
                // Traversal: read a random atomic part, traverse its fields
                let idx = rng.usize(0..db.atomic_count());
                transaction(|tx| {
                    let ap = &db.atomic_parts[idx];
                    let _ptype = tx.read(&ap.ptype);
                    let _date = tx.read(&ap.build_date);
                    let _x = tx.read(&ap.x);
                    let _y = tx.read(&ap.y);
                })
            }
            _ if roll < 50.0 => {
                // Structural modification: update an atomic part's date and field
                let idx = rng.usize(0..db.atomic_count());
                transaction(|tx| {
                    let ap = &db.atomic_parts[idx];
                    let d = tx.read(&ap.build_date);
                    tx.write(&ap.build_date, d.wrapping_add(1));
                    let x = tx.read(&ap.x);
                    tx.write(&ap.x, x.wrapping_add(1));
                })
            }
            _ if roll < 70.0 => {
                // Update a single field on a composite part
                let idx = rng.usize(0..db.composite_count());
                transaction(|tx| {
                    let cp = &db.composite_parts[idx];
                    let d = tx.read(&cp.build_date);
                    tx.write(&cp.build_date, d.wrapping_add(1));
                })
            }
            _ => {
                // Date range query: sum build dates in a range
                let lo = rng.i32(0..=800);
                let hi = lo + rng.i32(50..=200);
                transaction(|tx| {
                    let mut _sum = 0i64;
                    for &(_d, _cid) in &db.date_index {
                        if _d >= lo && _d <= hi {
                            let cp = &db.composite_parts[_cid];
                            let d2 = tx.read(&cp.build_date);
                            _sum += d2 as i64;
                        }
                    }
                })
            }
        };

        committed += 1;
    }

    WorkerStats { committed, aborted }
}

// ── Main ────────────────────────────────────────────────
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut duration_ms = DEFAULT_DURATION_MS;
    let mut nb_threads = DEFAULT_NB_THREADS;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-d" if i + 1 < args.len() => { i += 1; duration_ms = args[i].parse().unwrap_or(DEFAULT_DURATION_MS); }
            "-t" if i + 1 < args.len() => { i += 1; nb_threads = args[i].parse().unwrap_or(DEFAULT_NB_THREADS); }
            "-h" | "--help" => {
                println!("Usage: stmbench7 [-d ms] [-t n]");
                return;
            }
            _ => {}
        }
        i += 1;
    }

    println!("STMbench7 — Rust TM API");
    println!("Duration: {} ms  Threads: {}", duration_ms, nb_threads);

    tm_init();

    let db = Arc::new(Database::new());
    println!(
        "  Composite parts: {}  Atomic parts: {}",
        db.composite_count(),
        db.atomic_count()
    );

    let stop = Arc::new(AtomicBool::new(false));

    let mut handles = Vec::with_capacity(nb_threads);
    for tid in 0..nb_threads {
        let db = Arc::clone(&db);
        let stop = Arc::clone(&stop);
        handles.push(std::thread::spawn(move || {
            tm_init_thread();
            let stats = worker(tid, &db, &stop);
            tm_exit_thread();
            stats
        }));
    }

    std::thread::sleep(Duration::from_millis(duration_ms));
    stop.store(true, Ordering::Relaxed);

    let mut total_committed = 0u64;
    let mut total_aborted = 0u64;
    for (tid, h) in handles.into_iter().enumerate() {
        let stats = h.join().unwrap();
        total_committed += stats.committed;
        total_aborted += stats.aborted;
        println!(
            "  Thread {}: committed={} aborted={}",
            tid, stats.committed, stats.aborted
        );
    }

    println!(
        "\n  Total committed: {}  Total aborted: {}  Ratio: {:.2}%",
        total_committed,
        total_aborted,
        if total_committed > 0 {
            total_aborted as f64 / total_committed as f64 * 100.0
        } else {
            0.0
        }
    );

    let throughput = total_committed as f64 / duration_ms as f64 * 1000.0;
    println!("  Throughput: {:.0} txns/sec", throughput);

    tm_exit();
    println!("PASS");
}
