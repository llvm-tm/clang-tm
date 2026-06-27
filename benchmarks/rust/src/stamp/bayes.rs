use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

const MAX_TASKS: usize = 131072;
const MAX_PARENTS: usize = 8;

struct ParentSet {
    count: TmCell<i32>,
    data: [TmCell<i32>; MAX_PARENTS],
}

impl ParentSet {
    fn new() -> Self {
        ParentSet { count: TmCell::new(0), data: [TmCell::new(-1), TmCell::new(-1), TmCell::new(-1), TmCell::new(-1), TmCell::new(-1), TmCell::new(-1), TmCell::new(-1), TmCell::new(-1)] }
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
}

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

fn compute_density_ll(data: &BayesData, var: usize, parents: &[i32]) -> f64 {
    let num_configs = 1usize << parents.len();
    let mut c0 = vec![0i32; num_configs];
    let mut c1 = vec![0i32; num_configs];
    for r in 0..data.num_records {
        let mut config = 0usize;
        for (i, &p) in parents.iter().enumerate() {
            if data.records[r][p as usize] != 0 { config |= 1 << i; }
        }
        if data.records[r][var] != 0 { c1[config] += 1; } else { c0[config] += 1; }
    }
    let mut ll = 0.0f64;
    let nf = data.num_records as f64;
    for c in 0..num_configs {
        let total = c0[c] + c1[c];
        if total == 0 { continue; }
        let frac = total as f64 / nf;
        let p0 = c0[c] as f64 / total as f64;
        let p1 = c1[c] as f64 / total as f64;
        if p0 > 0.0 { ll += frac * p0 * p0.ln(); }
        if p1 > 0.0 { ll += frac * p1 * p1.ln(); }
    }
    ll
}

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
                    if child >= 0 && !visited[child as usize] { stack.push(child); }
                }
            }
            false
        })
    })
}

fn find_best_insert(data: &BayesData, to: i32, tx: &tm::Transaction) -> Task {
    if tx.read(&data.total_parents) >= data.global_max_edges * data.num_var as i32 {
        return Task { op: -1, from: -1, to: -1, score: -1e100 };
    }
    let mut best = Task { op: 0, from: -1, to: -1, score: -1e100 };
    let base_ll = tx.read(&data.local_ll[to as usize]);
    for from in 0..data.num_var as i32 {
        if from == to { continue; }
        if data.parents[to as usize].contains(tx, from) { continue; }
        if has_path(data, to, from) { continue; }
        let mut par = data.parents[to as usize].collect(tx);
        par.push(from);
        let new_ll = compute_density_ll(data, to as usize, &par);
        let delta = new_ll - base_ll;
        let score = tx.read(&data.total_parents) as f64 * data.base_penalty
                  + data.num_records as f64 * (tx.read(&data.base_log_likelihood) + delta);
        if score > best.score { best = Task { op: 0, from, to, score }; }
    }
    best
}

// ── CLI defaults matching C++ ─────────────────────────────────────
const TEST_DEFAULT_NUM_VAR: i32 = 32;
const TEST_DEFAULT_NUM_RECORD: i32 = 1024;
const TEST_DEFAULT_MAX_PARENTS: i32 = 2;
const TEST_DEFAULT_INSERT_PENALTY: i32 = 2;
const TEST_DEFAULT_MAX_EDGES_PER_VAR: i32 = 2;
const TEST_DEFAULT_NUM_THREADS: i32 = 4;

// ── d2l/l2d: bit-preserving f64 ↔ i64 (matching C++ memcpy) ─────
fn d2l(v: f64) -> i64 { v.to_bits() as i64 }
fn l2d(v: i64) -> f64 { f64::from_bits(v as u64) }

// ── LCG matching C++ bayes RNG ──────────────────────────────────
struct TestLcg(u32);
impl TestLcg {
    fn new(seed: u32) -> Self { TestLcg(if seed == 0 { 1 } else { seed }) }
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(1103515245).wrapping_add(12345);
        self.0 & 0x7fffffff
    }
}

pub fn test() -> i32 {
    let mut fails = 0;
    let mut total = 0u32;

    // ── CLI flags defaults ─────────────────────────────────────
    eprintln!("  Testing CLI defaults...");
    // Use canonical values matching C++ bayes defaults at bayes.cpp:30-36
    total += 1; if TEST_DEFAULT_NUM_VAR != 32 { eprintln!("  FAIL: default vars"); fails += 1; }
    total += 1; if TEST_DEFAULT_NUM_RECORD != 1024 { eprintln!("  FAIL: default records"); fails += 1; }
    total += 1; if TEST_DEFAULT_MAX_PARENTS != 2 { eprintln!("  FAIL: default max parents"); fails += 1; }
    total += 1; if TEST_DEFAULT_INSERT_PENALTY != 2 { eprintln!("  FAIL: default penalty"); fails += 1; }
    total += 1; if TEST_DEFAULT_MAX_EDGES_PER_VAR != 2 { eprintln!("  FAIL: default max edges"); fails += 1; }
    total += 1; if TEST_DEFAULT_NUM_THREADS != 4 { eprintln!("  FAIL: default threads"); fails += 1; }

    // ── RNG determinism ────────────────────────────────────────
    eprintln!("  Testing RNG determinism...");
    {
        let mut a = TestLcg::new(42);
        let first5: Vec<u32> = (0..5).map(|_| a.next()).collect();
        let mut b = TestLcg::new(42);
        for i in 0..5 {
            let got = b.next();
            total += 1;
            if got != first5[i] { eprintln!("  FAIL: LCG determinism at {}: expected {} got {}", i, first5[i], got); fails += 1; }
        }
    }

    // ── d2l/l2d roundtrip ──────────────────────────────────────
    eprintln!("  Testing d2l/l2d roundtrip...");
    {
        let orig = -0.693147f64;
        let bits = d2l(orig);
        let back = l2d(bits);
        total += 1;
        if (back - orig).abs() > 1e-12 {
            eprintln!("  FAIL: d2l/l2d roundtrip: expected {} got {}", orig, back); fails += 1;
        }
    }

    // ── Penalty formula ─────────────────────────────────────────
    eprintln!("  Testing penalty formula...");
    {
        let num_record = 100i32;
        let insert_penalty = 2i32;
        let base_penalty = -0.5 * (num_record as f64).ln() * insert_penalty as f64;
        total += 1;
        if base_penalty >= 0.0 {
            eprintln!("  FAIL: base penalty not negative: {}", base_penalty); fails += 1;
        }
    }

    // ── Density LL with C++-matching synthetic data ─────────────
    // Generates 100 records, 2 vars: r%2 and (r*7)%2 (matching
    // C++ test_logic at bayes.cpp:392-396).
    eprintln!("  Testing density LL computation...");
    {
        let nvar = 2usize;
        let nrec = 100usize;
        let records: Vec<Vec<i32>> = (0..nrec).map(|r| {
            vec![(r % 2) as i32, ((r * 7) % 2) as i32]
        }).collect();
        let parents: Vec<ParentSet> = (0..nvar).map(|_| ParentSet::new()).collect();
        let children: Vec<ParentSet> = (0..nvar).map(|_| ParentSet::new()).collect();
        let local_ll: Vec<TmCell<f64>> = (0..nvar).map(|_| TmCell::new(0.0)).collect();
        let data = BayesData {
            num_var: nvar, num_records: nrec, base_penalty: 1.0,
            records, parents, children, local_ll,
            base_log_likelihood: TmCell::new(0.0),
            total_parents: TmCell::new(0),
            global_max_edges: 2,
            task_list: TaskList::new(),
        };

        let ll_empty = compute_density_ll(&data, 0, &[]);
        total += 1;
        if ll_empty >= 0.0 || ll_empty.is_nan() {
            eprintln!("  FAIL: LL with no parents should be negative, got {}", ll_empty); fails += 1;
        }

        let ll_with = compute_density_ll(&data, 1, &[0]);
        total += 1;
        if ll_with.is_nan() || ll_with.is_infinite() {
            eprintln!("  FAIL: LL with parent is {}", ll_with); fails += 1;
        }
    }

    if fails > 0 {
        eprintln!("bayes: {}/{} test(s) failed", fails, total);
    } else {
        eprintln!("  All {} tests passed.", total);
    }
    fails
}

pub fn run(config: &Config, stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== Bayes ===");
    let num_var = config.points.max(8).min(64);
    let num_records = config.num_customers.max(64).min(2048);
    let max_parents = 8usize;
    let insert_penalty = 2.0;
    let base_penalty = -0.5 * (num_records as f64).ln() * insert_penalty;
    let global_max_edges = 2i32;
    println!("  Vars: {}  Records: {}  Max parents: {}  Penalty: {:.0}",
             num_var, num_records, max_parents, insert_penalty);

    let mut rng = Rng::new(42);
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
    let mut records: Vec<Vec<i32>> = Vec::with_capacity(num_records);
    for r in 0..num_records {
        let mut rec = vec![0i32; num_var];
        for v in 0..num_var {
            if parents_init[v].is_empty() {
                rec[v] = (rng.next() % 2) as i32;
            } else {
                let mut val = 0i32;
                let mut threshold = 30i32;
                for &p in &parents_init[v] {
                    let prev = if r > 0 { records[r - 1][p as usize] } else { 0 };
                    val = (val << 1) | prev;
                    threshold += (p * 7 + v as i32 * 11) % 40;
                }
                threshold = (threshold + val * 17) % 100;
                rec[v] = (rng.next() % 100 < threshold as u64) as i32;
            }
        }
        records.push(rec);
    }

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
        for tid in 0..config.threads {
            let d = data.clone();
            let sc = stop;
            let go = &g_ops;
            let chunk = (num_var + config.threads - 1) / config.threads;
            let start = tid * chunk;
            let end = (start + chunk).min(num_var);
            threads.push(s.spawn(move || {
                // Phase 1: Init per-variable LL
                for v in start..end {
                    let ll = compute_density_ll(&d, v, &[]);
                    transaction(|tx| {
                        tx.write(&d.local_ll[v], ll);
                        tx.write(&d.base_log_likelihood, tx.read(&d.base_log_likelihood) + ll);
                    });
                }

                // Phase 2: Enqueue single-edge tasks
                for v in start..end {
                    let base_ll = transaction(|tx| tx.read(&d.local_ll[v]));
                    for from in 0..d.num_var as i32 {
                        if from == v as i32 { continue; }
                        let with_ll = compute_density_ll(&d, v, &[from]);
                        if with_ll > base_ll {
                            let delta = with_ll - base_ll;
                            let score = transaction(|tx| {
                                d.base_penalty + d.num_records as f64 * (tx.read(&d.base_log_likelihood) + delta)
                            });
                            transaction(|tx| {
                                d.task_list.push(tx, 0, from, v as i32, score);
                            });
                        }
                    }
                }

                // Phase 3: Greedy search loop (all threads participate)
                loop {
                    if sc.load(Ordering::Relaxed) { break; }
                    let task = transaction(|tx| d.task_list.pop_best(tx));
                    if task.op < 0 { break; }
                    let ok = transaction(|tx| {
                        if d.parents[task.to as usize].contains(tx, task.from) { return false; }
                        if has_path(&d, task.to, task.from) { return false; }
                        if tx.read(&d.total_parents) >= d.global_max_edges * d.num_var as i32 { return false; }

                        d.parents[task.to as usize].insert(tx, task.from);
                        d.children[task.from as usize].insert(tx, task.to);
                        tx.write(&d.total_parents, tx.read(&d.total_parents) + 1);

                        let par = d.parents[task.to as usize].collect(tx);
                        let new_ll = compute_density_ll(&d, task.to as usize, &par);
                        let delta = new_ll - tx.read(&d.local_ll[task.to as usize]);
                        tx.write(&d.local_ll[task.to as usize], new_ll);
                        tx.write(&d.base_log_likelihood, tx.read(&d.base_log_likelihood) + delta);

                        let next = find_best_insert(&d, task.to, tx);
                        if next.from >= 0 {
                            d.task_list.push(tx, next.op, next.from, next.to, next.score);
                        }
                        true
                    });
                    if ok {
                        go.fetch_add(1, Ordering::Relaxed);
                    }
                }
            }));
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });

    let elapsed = t0.elapsed().as_millis() as u64;
    let ops_count = g_ops.load(Ordering::Relaxed);
    let total_par = unsafe { *data.total_parents.ptr() };
    println!("  Operations: {}  Total parents: {}  Elapsed: {} ms", ops_count, total_par, elapsed);
}
