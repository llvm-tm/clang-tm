// ── STAMP Benchmarks: KMeans, Labyrinth, Vacation (Rust/TM) ──────────
//
// Port of selected STAMP v0.9.10 workloads.
// Original: https://stamp.stanford.edu/

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;

use tm::{transaction, TmCell, Transaction};

fn parse_args() -> Config {
    let args: Vec<String> = std::env::args().collect();
    let mut c = Config::default();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-t" if i+1<args.len() => { c.threads = args[i+1].parse().unwrap_or(4); i+=2; }
            "-d" if i+1<args.len() => { c.duration = args[i+1].parse().unwrap_or(10000); i+=2; }
            "-b" if i+1<args.len() => {
                c.bench = match args[i+1].to_lowercase().as_str() {
                    "kmeans" => Bench::KMeans, "labyrinth" => Bench::Labyrinth,
                    "vacation" => Bench::Vacation, _ => Bench::All,
                };
                i+=2;
            }
            "--points" if i+1<args.len() => { c.points = args[i+1].parse().unwrap_or(10000); i+=2; }
            "--clusters" if i+1<args.len() => { c.clusters = args[i+1].parse().unwrap_or(5); i+=2; }
            "--dims" if i+1<args.len() => { c.dims = args[i+1].parse().unwrap_or(3); i+=2; }
            "--grid-x" if i+1<args.len() => { c.grid_x = args[i+1].parse().unwrap_or(5); i+=2; }
            "--grid-y" if i+1<args.len() => { c.grid_y = args[i+1].parse().unwrap_or(5); i+=2; }
            "--grid-z" if i+1<args.len() => { c.grid_z = args[i+1].parse().unwrap_or(5); i+=2; }
            "--num-rooms" if i+1<args.len() => { c.num_rooms = args[i+1].parse().unwrap_or(10); i+=2; }
            "--num-customers" if i+1<args.len() => { c.num_customers = args[i+1].parse().unwrap_or(100); i+=2; }
            "--num-items" if i+1<args.len() => { c.num_items = args[i+1].parse().unwrap_or(10); i+=2; }
            _ => i+=1,
        }
    }
    c
}

#[derive(Clone, Copy, PartialEq)]
enum Bench { All, KMeans, Labyrinth, Vacation }

#[derive(Clone)]
struct Config {
    threads: usize, duration: usize, bench: Bench,
    points: usize, clusters: usize, dims: usize,
    grid_x: usize, grid_y: usize, grid_z: usize,
    num_rooms: usize, num_customers: usize, num_items: usize,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            threads: 4, duration: 10000, bench: Bench::All,
            points: 10000, clusters: 5, dims: 3,
            grid_x: 5, grid_y: 5, grid_z: 5,
            num_rooms: 10, num_customers: 100, num_items: 10,
        }
    }
}

struct Rng(u64);
impl Rng {
    fn new(seed: u64) -> Self { Self(seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407)) }
    fn next(&mut self) -> u64 { self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); self.0 >> 33 }
    fn range(&mut self, lo: u64, hi: u64) -> f64 { lo as f64 + (self.next() % (hi - lo)) as f64 }
}

// ═══════════════════════════════════════════════════════════════════════
// KMeans
// ═══════════════════════════════════════════════════════════════════════

struct KMeansState {
    points: Vec<Vec<f64>>,           // dims x points
    clusters: Vec<Vec<f64>>,         // dims x clusters
    assignment: Vec<TmCell<i32>>,    // per-point cluster assignment
    new_count: Vec<TmCell<i32>>,     // per-cluster point count
    new_sum: Vec<Vec<TmCell<f64>>>,  // per-cluster dim sum
    config: Arc<Config>,
}

impl KMeansState {
    fn new(config: &Config) -> Self {
        let mut rng = Rng::new(42);
        let points: Vec<Vec<f64>> = (0..config.dims).map(|_|
            (0..config.points).map(|_| (rng.next() as f64 % 1000.0) / 100.0).collect()
        ).collect();
        let clusters: Vec<Vec<f64>> = (0..config.dims).map(|_|
            (0..config.clusters).map(|_| (rng.next() as f64 % 1000.0) / 100.0).collect()
        ).collect();
        let assignment = (0..config.points).map(|_| TmCell::new(-1)).collect();
        let new_count = (0..config.clusters).map(|_| TmCell::new(0)).collect();
        let new_sum = (0..config.dims).map(|_|
            (0..config.clusters).map(|_| TmCell::new(0.0f64)).collect()
        ).collect();
        KMeansState { points, clusters, assignment, new_count, new_sum, config: Arc::new(config.clone()) }
    }
}

fn kmeans_worker(
    tid: usize, state: &KMeansState, stop: &AtomicBool, ops: &AtomicU64,
) {
    let config = &state.config;
    let chunk = config.points / config.threads;
    let start = tid * chunk;
    let end = if tid == config.threads - 1 { config.points } else { start + chunk };

    while !stop.load(Ordering::Relaxed) {
        // Assignment phase
        for p in start..end {
            let mut best_cluster = 0i32;
            let mut best_dist = f64::MAX;
            for c in 0..config.clusters {
                let mut dist = 0.0;
                for d in 0..config.dims {
                    let diff = state.points[d][p] - state.clusters[d][c];
                    dist += diff * diff;
                }
                if dist < best_dist { best_dist = dist; best_cluster = c as i32; }
            }
            unsafe { *state.assignment[p].ptr() = best_cluster; }
        }

        // Reset accumulators
        for c in 0..config.clusters {
            unsafe { *state.new_count[c].ptr() = 0; }
            for d in 0..config.dims {
                unsafe { *state.new_sum[d][c].ptr() = 0.0; }
            }
        }

        // Accumulate phase (TM)
        transaction(|tx| {
            for p in start..end {
                let cluster = unsafe { *state.assignment[p].ptr() };
                tx.write(&state.new_count[cluster as usize],
                         tx.read(&state.new_count[cluster as usize]) + 1);
                for d in 0..config.dims {
                    tx.write(&state.new_sum[d][cluster as usize],
                             tx.read(&state.new_sum[d][cluster as usize]) + state.points[d][p]);
                }
            }
        });

        ops.fetch_add(1, Ordering::Relaxed);
    }
}

fn run_kmeans(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== KMeans ===");
    let state = KMeansState::new(config);
    let dur = std::time::Duration::from_millis(config.duration as u64);
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let sref = &state;
            s.spawn(move || {
                kmeans_worker(tid, sref, stop, ops);
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    println!("  Final centroids computed (KMeans complete)");
}

// ═══════════════════════════════════════════════════════════════════════
// Labyrinth
// ═══════════════════════════════════════════════════════════════════════

struct LabyrinthGrid {
    grid: Vec<TmCell<i32>>,
    x: usize, y: usize, z: usize,
}

impl LabyrinthGrid {
    fn new(x: usize, y: usize, z: usize) -> Self {
        let size = x * y * z;
        LabyrinthGrid {
            grid: (0..size).map(|_| TmCell::new(0)).collect(),
            x, y, z,
        }
    }

    fn idx(&self, ix: usize, iy: usize, iz: usize) -> usize {
        iz * self.x * self.y + iy * self.x + ix
    }

    fn is_free(&self, tx: &Transaction, ix: usize, iy: usize, iz: usize) -> bool {
        tx.read(&self.grid[self.idx(ix, iy, iz)]) == 0
    }

    fn set(&self, tx: &Transaction, ix: usize, iy: usize, iz: usize, val: i32) {
        tx.write(&self.grid[self.idx(ix, iy, iz)], val);
    }
}

fn labyrinth_worker(
    tid: usize, grid: &LabyrinthGrid, config: &Config,
    stop: &AtomicBool, ops: &AtomicU64,
) {
    let rng = std::cell::RefCell::new(Rng::new(tid as u64 * 12345 + 42));
    let paths_per_tx = 2;

    while !stop.load(Ordering::Relaxed) {
        for _ in 0..paths_per_tx {
            let sx = (rng.borrow_mut().next() % config.grid_x as u64) as usize;
            let sy = (rng.borrow_mut().next() % config.grid_y as u64) as usize;
            let sz = (rng.borrow_mut().next() % config.grid_z as u64) as usize;
            let dx = (rng.borrow_mut().next() % config.grid_x as u64) as usize;
            let dy = (rng.borrow_mut().next() % config.grid_y as u64) as usize;
            let dz = (rng.borrow_mut().next() % config.grid_z as u64) as usize;

            let found = transaction(|tx| {
                if !grid.is_free(tx, sx, sy, sz) || !grid.is_free(tx, dx, dy, dz) {
                    return false;
                }
                // Simple BFS (no queue for Rust — use grid as visited)
                let mut queue = Vec::with_capacity(config.grid_x * config.grid_y * config.grid_z);
                queue.push((sx, sy, sz, 0i32));
                grid.set(tx, sx, sy, sz, 1);
                let mut path_found = false;

                let mut front = 0;
                while front < queue.len() && !path_found {
                    let (cx, cy, cz, dist) = queue[front];
                    front += 1;
                    if dist > 10 { break; } // limit path length

                    let dirs = [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)];
                    for &(ddx, ddy, ddz) in &dirs {
                        let nx = cx.wrapping_add(ddx as usize);
                        let ny = cy.wrapping_add(ddy as usize);
                        let nz = cz.wrapping_add(ddz as usize);
                        if nx >= config.grid_x || ny >= config.grid_y || nz >= config.grid_z {
                            continue;
                        }
                        if nx == dx && ny == dy && nz == dz {
                            path_found = true;
                            break;
                        }
                        if grid.is_free(tx, nx, ny, nz) {
                            grid.set(tx, nx, ny, nz, 1);
                            queue.push((nx, ny, nz, dist + 1));
                        }
                    }
                }
                path_found
            });
            let _ = found;
        }
        ops.fetch_add(1, Ordering::Relaxed);
    }
}

fn run_labyrinth(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Labyrinth ===");
    println!("  Grid: {}x{}x{}", config.grid_x, config.grid_y, config.grid_z);
    let grid = Arc::new(LabyrinthGrid::new(config.grid_x, config.grid_y, config.grid_z));
    let dur = std::time::Duration::from_millis(config.duration as u64);
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let g = grid.clone();
            s.spawn(move || {
                labyrinth_worker(tid, &g, config, stop, ops);
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    println!("  Labyrinth complete");
}

// ═══════════════════════════════════════════════════════════════════════
// Vacation
// ═══════════════════════════════════════════════════════════════════════

struct ReservationManager {
    rooms: Vec<TmCell<i32>>,
    cars: Vec<TmCell<i32>>,
    flights: Vec<TmCell<i32>>,
    customers: Vec<TmCell<i64>>,  // customer balance
    num_rooms: usize, num_cars: usize, num_flights: usize,
}

impl ReservationManager {
    fn new(config: &Config) -> Self {
        ReservationManager {
            rooms: (0..config.num_rooms).map(|_| TmCell::new(100)).collect(),
            cars: (0..config.num_items).map(|_| TmCell::new(100)).collect(),
            flights: (0..config.num_items).map(|_| TmCell::new(100)).collect(),
            customers: (0..config.num_customers).map(|_| TmCell::new(1000)).collect(),
            num_rooms: config.num_rooms, num_cars: config.num_items, num_flights: config.num_items,
        }
    }
}

fn vacation_worker(
    tid: usize, rm: &ReservationManager, config: &Config,
    stop: &AtomicBool, ops: &AtomicU64,
) {
    let rng = std::cell::RefCell::new(Rng::new(tid as u64 * 12345 + 42));

    while !stop.load(Ordering::Relaxed) {
        let choice = rng.borrow_mut().next() % 100;
        let customer = (rng.borrow_mut().next() % config.num_customers as u64) as usize;

        // Pre-generate all random values for all branches
        let make_rtype = rng.borrow_mut().next() % 3;
        let make_item = (rng.borrow_mut().next() % 100) as usize;
        let cancel_rtype = rng.borrow_mut().next() % 3;
        let cancel_item = (rng.borrow_mut().next() % 100) as usize;

        transaction(|tx| {
            match choice {
                0..=39 => {
                    let cost = 50i64;
                    let bal = tx.read(&rm.customers[customer]) - cost;
                    if bal >= 0 {
                        tx.write(&rm.customers[customer], bal);
                        let remaining = match make_rtype {
                            0 => tx.read(&rm.rooms[make_item % rm.num_rooms]) - 1,
                            1 => tx.read(&rm.cars[make_item % rm.num_cars]) - 1,
                            _ => tx.read(&rm.flights[make_item % rm.num_flights]) - 1,
                        };
                        match make_rtype {
                            0 => tx.write(&rm.rooms[make_item % rm.num_rooms], remaining),
                            1 => tx.write(&rm.cars[make_item % rm.num_cars], remaining),
                            _ => tx.write(&rm.flights[make_item % rm.num_flights], remaining),
                        }
                    }
                }
                40..=79 => {
                    let refund = 50i64;
                    tx.write(&rm.customers[customer], tx.read(&rm.customers[customer]) + refund);
                    match cancel_rtype {
                        0 => tx.write(&rm.rooms[cancel_item % rm.num_rooms], tx.read(&rm.rooms[cancel_item % rm.num_rooms]) + 1),
                        1 => tx.write(&rm.cars[cancel_item % rm.num_cars], tx.read(&rm.cars[cancel_item % rm.num_cars]) + 1),
                        _ => tx.write(&rm.flights[cancel_item % rm.num_flights], tx.read(&rm.flights[cancel_item % rm.num_flights]) + 1),
                    }
                }
                _ => {
                    let _bal = tx.read(&rm.customers[customer]);
                }
            }
        });

        ops.fetch_add(1, Ordering::Relaxed);
    }
}

fn run_vacation(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Vacation ===");
    let rm = Arc::new(ReservationManager::new(config));
    let dur = std::time::Duration::from_millis(config.duration as u64);
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let r = rm.clone();
            s.spawn(move || {
                vacation_worker(tid, &r, config, stop, ops);
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    println!("  Vacation complete");
}

// ═══════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════

fn main() {
    tm::tm_init();
    let config = parse_args();

    let bench_name = match config.bench {
        Bench::All => "all", Bench::KMeans => "kmeans",
        Bench::Labyrinth => "labyrinth", Bench::Vacation => "vacation",
    };
    println!("========= STAMP Benchmarks =========");
    println!("Bench: {bench_name}  Threads: {t}  Duration: {d}ms",
             t=config.threads, d=config.duration);
    println!();

    if config.bench == Bench::All || config.bench == Bench::KMeans {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        run_kmeans(&config, &stop, &ops);
    }

    if config.bench == Bench::All || config.bench == Bench::Labyrinth {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        run_labyrinth(&config, &stop, &ops);
    }

    if config.bench == Bench::All || config.bench == Bench::Vacation {
        let stop = AtomicBool::new(false);
        let ops = AtomicU64::new(0);
        run_vacation(&config, &stop, &ops);
    }

    println!("\nTM aborts: {}", tm::tm_abort_count());
    tm::tm_exit();
}
