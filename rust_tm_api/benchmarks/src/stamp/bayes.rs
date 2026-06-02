use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread_local;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

const MAX_TASKS: usize = 131072;
const MAX_PARENTS: usize = 2;

// ── Small fixed-size parent set (TM-tracked) ──────────────
struct ParentSet {
    count: TmCell<i32>,
    data: [TmCell<i32>; MAX_PARENTS],
}

impl ParentSet {
    fn new() -> Self {
        ParentSet { count: TmCell::new(0), data: [TmCell::new(-1), TmCell::new(-1)] }
    }
    fn contains(&self, tx: &tm::Transaction, val: i32) -> bool {
        let n = tx.read(&self.count);
        for i in 0..n as usize {
            if tx.read(&self.data[i]) == val { return true; }
        }
        false
    }
    fn insert(&self, tx: &tm::Transaction, val: i32) {
        let n = tx.read(&self.count) as usize;
        if n < MAX_PARENTS {
            tx.write(&self.data[n], val);
            tx.write(&self.count, (n + 1) as i32);
        }
    }
    fn peek_count(&self) -> i32 { unsafe { *self.count.ptr() } }
    fn peek_get(&self, i: usize) -> i32 { unsafe { *self.data[i].ptr() } }
    fn peek_insert(&self, val: i32) {
        let n = self.peek_count() as usize;
        if n < MAX_PARENTS {
            unsafe { *self.data[n].ptr() = val; }
            unsafe { *self.count.ptr() = (n + 1) as i32; }
        }
    }
    fn collect(&self, tx: &tm::Transaction) -> Vec<i32> {
        let n = tx.read(&self.count) as usize;
        (0..n).map(|i| tx.read(&self.data[i])).collect()
    }
}

// ── Task list (TM priority queue) ─────────────────────────
struct TaskList {
    scores: Vec<TmCell<f64>>,
    ops: Vec<TmCell<i32>>,
    from_ids: Vec<TmCell<i32>>,
    to_ids: Vec<TmCell<i32>>,
    count: TmCell<i32>,
}

#[derive(Clone)]
struct Task { op: i32, from: i32, to: i32, score: f64 }

impl TaskList {
    fn new() -> Self {
        let n = MAX_TASKS;
        let mut scores = Vec::with_capacity(n);
        let mut ops = Vec::with_capacity(n);
        let mut from_ids = Vec::with_capacity(n);
        let mut to_ids = Vec::with_capacity(n);
        for _ in 0..n {
            scores.push(TmCell::new(0.0));
            ops.push(TmCell::new(0));
            from_ids.push(TmCell::new(0));
            to_ids.push(TmCell::new(0));
        }
        TaskList { scores, ops, from_ids, to_ids, count: TmCell::new(0) }
    }
    fn push(&self, tx: &tm::Transaction, op: i32, from: i32, to: i32, score: f64) {
        let i = tx.read(&self.count) as usize;
        if i >= MAX_TASKS { return; }
        tx.write(&self.scores[i], score);
        tx.write(&self.ops[i], op);
        tx.write(&self.from_ids[i], from);
        tx.write(&self.to_ids[i], to);
        tx.write(&self.count, (i + 1) as i32);
    }
    fn push_setup(&self, op: i32, from: i32, to: i32, score: f64) {
        let i = unsafe { *self.count.ptr() } as usize;
        if i >= MAX_TASKS { return; }
        unsafe { *self.scores[i].ptr() = score; }
        unsafe { *self.ops[i].ptr() = op; }
        unsafe { *self.from_ids[i].ptr() = from; }
        unsafe { *self.to_ids[i].ptr() = to; }
        unsafe { *self.count.ptr() = (i + 1) as i32; }
    }
    fn pop_best(&self, tx: &tm::Transaction) -> Task {
        let n = tx.read(&self.count) as usize;
        if n == 0 { return Task { op: -1, from: -1, to: -1, score: -1e100 }; }
        let mut best = 0usize;
        let mut best_score = tx.read(&self.scores[0]);
        for i in 1..n {
            let s = tx.read(&self.scores[i]);
            if s > best_score { best_score = s; best = i; }
        }
        let last = n - 1;
        let t = Task {
            op: tx.read(&self.ops[best]),
            from: tx.read(&self.from_ids[best]),
            to: tx.read(&self.to_ids[best]),
            score: best_score,
        };
        if best != last {
            tx.write(&self.scores[best], tx.read(&self.scores[last]));
            tx.write(&self.ops[best], tx.read(&self.ops[last]));
            tx.write(&self.from_ids[best], tx.read(&self.from_ids[last]));
            tx.write(&self.to_ids[best], tx.read(&self.to_ids[last]));
        }
        tx.write(&self.count, last as i32);
        t
    }
    fn peek_empty(&self) -> bool { let v = unsafe { *self.count.ptr() }; v == 0 }
}

// ── Bayes data ────────────────────────────────────────────
struct BayesData {
    num_var: usize,
    num_records: usize,
    base_penalty: f64,
    records: Vec<Vec<i32>>,
    parents: Vec<ParentSet>,
    children: Vec<ParentSet>,
    local_ll: Vec<TmCell<f64>>,
    base_log_likelihood: TmCell<f64>,
    total_parents: TmCell<i32>,
    global_max_edges: i32,
    task_list: TaskList,
}

// ── Compute density log-likelihood for a variable with given parents ─
fn compute_density_ll(data: &BayesData, var: usize, parents: &[i32]) -> f64 {
    let num_configs = 1usize << parents.len();
    let mut cfg = vec![(0i32, 0i32); num_configs];
    for r in 0..data.num_records {
        let mut config = 0usize;
        for (i, &p) in parents.iter().enumerate() {
            if data.records[r][p as usize] != 0 { config |= 1 << i; }
        }
        if data.records[r][var] != 0 {
            cfg[config].1 += 1;
        } else {
            cfg[config].0 += 1;
        }
    }
    let mut ll = 0.0f64;
    let nf = data.num_records as f64;
    for c in 0..num_configs {
        let total = cfg[c].0 + cfg[c].1;
        if total == 0 { continue; }
        let frac = total as f64 / nf;
        let p0 = cfg[c].0 as f64 / total as f64;
        let p1 = cfg[c].1 as f64 / total as f64;
        if p0 > 0.0 { ll += frac * p0 * p0.ln(); }
        if p1 > 0.0 { ll += frac * p1 * p1.ln(); }
    }
    ll
}

// ── Cycle detection (DFS, reads committed child sets) ─────
fn has_path(data: &BayesData, from: i32, to: i32) -> bool {
    thread_local! {
        static VISITED: RefCell<Vec<bool>> = RefCell::new(Vec::new());
        static STACK: RefCell<Vec<i32>> = RefCell::new(Vec::new());
    }
    VISITED.with(|v| {
        let mut visited = v.borrow_mut();
        visited.clear();
        visited.resize(data.num_var, false);
        STACK.with(|s| {
            let mut stack = s.borrow_mut();
            stack.clear();
            stack.push(from);
            while let Some(cur) = stack.pop() {
                if cur == to { return true; }
                if cur as usize >= data.num_var { continue; }
                if visited[cur as usize] { continue; }
                visited[cur as usize] = true;
                let n = data.children[cur as usize].peek_count();
                for i in 0..n as usize {
                    let child = data.children[cur as usize].peek_get(i);
                    if child >= 0 && !visited[child as usize] {
                        stack.push(child);
                    }
                }
            }
            false
        })
    })
}

// ── Find best insertion for a variable ────────────────────
fn find_best_insert(data: &BayesData, to: i32, tx: &tm::Transaction) -> Task {
    if tx.read(&data.total_parents) >= data.global_max_edges * data.num_var as i32 {
        return Task { op: -1, from: -1, to: -1, score: -1e100 };
    }
    let mut best = Task { op: 0, from: -1, to: -1, score: -1e100 };
    for from in 0..data.num_var as i32 {
        if from == to { continue; }
        if data.parents[to as usize].contains(tx, from) { continue; }
        if has_path(data, to, from) { continue; }
        let mut par = data.parents[to as usize].collect(tx);
        par.push(from);
        let new_ll = compute_density_ll(data, to as usize, &par);
        let delta = new_ll - tx.read(&data.local_ll[to as usize]);
        let score = tx.read(&data.total_parents) as f64 * data.base_penalty
                  + data.num_records as f64 * (tx.read(&data.base_log_likelihood) + delta);
        if score > best.score {
            best = Task { op: 0, from, to, score };
        }
    }
    best
}

// ── Worker function ───────────────────────────────────────
pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Bayes ===");
    let num_var = config.points.max(8).min(64);
    let num_records = config.num_customers.max(64).min(2048);
    let max_parents = 2usize;
    let insert_penalty = 2.0;
    let base_penalty = -0.5 * (num_records as f64).ln() * insert_penalty;
    let global_max_edges = 2i32;
    println!("  Vars: {}  Records: {}  Max parents: {}  Penalty: {:.0}",
             num_var, num_records, max_parents, insert_penalty);

    // ── Data generation ──────────────────────────────────
    let mut rng = Rng::new(42);
    // Generate random parent graph
    let mut parents_init: Vec<Vec<i32>> = (0..num_var).map(|_| Vec::new()).collect();
    for v in 1..num_var {
        let n = (rng.next() as usize % max_parents.min(v)) + 1;
        for _ in 0..n {
            loop {
                let p = (rng.next() as usize) % v;
                if !parents_init[v].contains(&(p as i32)) {
                    parents_init[v].push(p as i32);
                    break;
                }
            }
        }
    }
    // Generate random records
    let records: Vec<Vec<i32>> = (0..num_records).map(|_| {
        (0..num_var).map(|v| {
            if parents_init[v].is_empty() {
                (rng.next() % 2) as i32
            } else {
                let mut key = rng.next();
                for &p in &parents_init[v] {
                    key = key.wrapping_mul(31).wrapping_add(key);
                }
                (key % 2) as i32
            }
        }).collect()
    }).collect();

    // ── Allocate TM data structures ──────────────────────
    let parents: Vec<ParentSet> = (0..num_var).map(|_| ParentSet::new()).collect();
    let children: Vec<ParentSet> = (0..num_var).map(|_| ParentSet::new()).collect();
    let local_ll: Vec<TmCell<f64>> = (0..num_var).map(|_| TmCell::new(0.0)).collect();
    let base_log_likelihood = TmCell::new(0.0);
    let total_parents = TmCell::new(0);
    let task_list = TaskList::new();
    let data = Arc::new(BayesData {
        num_var, num_records, base_penalty,
        records: records.clone(),
        parents, children, local_ll,
        base_log_likelihood, total_parents,
        global_max_edges, task_list,
    });

    // Initialize parent sets from generated graph (non-TM)
    for v in 0..num_var {
        for &p in &parents_init[v] {
            data.parents[v].peek_insert(p);
            data.children[p as usize].peek_insert(v as i32);
        }
    }

    let dur = std::time::Duration::from_millis(config.duration as u64);
    let g_ops = AtomicU64::new(0);
    let t0 = std::time::Instant::now();

    std::thread::scope(|s| {
        let mut threads = Vec::new();
        for tid in 0..config.threads.max(1).min(2) { // at most 2 threads for bayes
            let d = data.clone();
            let sc = stop;
            let so = ops.clone();
            let go = &g_ops;
            let chunk = num_var / config.threads.max(1);
            let start = tid * chunk;
            let end = if tid == config.threads - 1 { num_var } else { (start + chunk).min(num_var) };
            threads.push(s.spawn(move || {
                if tid == 0 {
                    // Phase 1: Initial per-variable LL (just thread 0)
                    // Already initialized in setup, nothing to do here.
                }

                // Phase 2: Enqueue single-edge tasks (parallel)
                if tid == 0 && end > start {
                    // Phase 2 + Phase 3 happen in thread 0
                    thread2(&d, start, end, go, sc, so);
                }
            }));
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;
    let ops_count = g_ops.load(Ordering::Relaxed);
    let total_par = unsafe { *data.total_parents.ptr() };
    println!("  Operations: {}  Total parents: {}  Elapsed: {} ms",
             ops_count, total_par, elapsed);
}

fn thread2(data: &BayesData, _start: usize, _end: usize,
           g_ops: &AtomicU64, stop: &AtomicBool, _ops: &AtomicU64) {
    // Initialize per-variable LL
    let mut init_rng = Rng::new(42);
    let _ = &mut init_rng;
    let mut base_ll = 0.0f64;
    for v in 0..data.num_var {
        let ll = compute_density_ll(data, v, &[]);
        transaction(|tx| {
            tx.write(&data.local_ll[v], ll);
            tx.write(&data.base_log_likelihood, tx.read(&data.base_log_likelihood) + ll);
        });
        base_ll += ll;
    }
    // Enqueue single-edge tasks (Phase 2)
    for v in 0..data.num_var {
        for from in 0..data.num_var as i32 {
            if from == v as i32 { continue; }
            let with_ll = compute_density_ll(data, v, &[from]);
            if with_ll > base_ll / data.num_var as f64 {
                let delta = with_ll - base_ll / data.num_var as f64;
                let score = data.base_penalty + data.num_records as f64 * (base_ll + delta);
                transaction(|tx| {
                    data.task_list.push(tx, 0, from, v as i32, score);
                });
            }
        }
    }
    // Phase 3: Greedy search loop
    while !stop.load(Ordering::Relaxed) {
        // Pop best task
        let task = transaction(|tx| data.task_list.pop_best(tx));
        if task.op < 0 { break; }

        // Validate and apply
        transaction(|tx| {
            if data.parents[task.to as usize].contains(tx, task.from) { return; }
            if has_path(data, task.to, task.from) { return; }
            if tx.read(&data.total_parents) >= data.global_max_edges * data.num_var as i32 { return; }

            data.parents[task.to as usize].insert(tx, task.from);
            data.children[task.from as usize].insert(tx, task.to);
            tx.write(&data.total_parents, tx.read(&data.total_parents) + 1);

            // Recompute LL with new parent set
            let par = data.parents[task.to as usize].collect(tx);
            let new_ll = compute_density_ll(data, task.to as usize, &par);
            let delta = new_ll - tx.read(&data.local_ll[task.to as usize]);
            tx.write(&data.local_ll[task.to as usize], new_ll);
            tx.write(&data.base_log_likelihood, tx.read(&data.base_log_likelihood) + delta);

            // Find next best for same variable
            let next = find_best_insert(data, task.to, tx);
            if next.from >= 0 {
                data.task_list.push(tx, next.op, next.from, next.to, next.score);
            }
        });
        g_ops.fetch_add(1, Ordering::Relaxed);
    }
}
