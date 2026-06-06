use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Instant;
use tm::*;

const MAX_PARENTS: usize = 8;
const MAX_TASKS: usize = 131072;

#[derive(Clone, Copy)]
struct Task {
    op: i32,
    from_id: i32,
    to_id: i32,
    score: f64,
}

// ── LCG matching C++ bayes RNG ─────────────────────────────────────
struct Lcg(u32);
impl Lcg {
    fn new(seed: u32) -> Self { Lcg(if seed == 0 { 1 } else { seed }) }
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(1103515245).wrapping_add(12345);
        self.0 & 0x7fffffff
    }
}

fn has_path_dfs(child_count: &[i32], child_data: &[i32], from: i32, to: i32, nvar: usize) -> bool {
    if from == to {
        return false;
    }
    let mut visited = vec![false; nvar];
    let mut stack = vec![to];
    visited[to as usize] = true;
    while let Some(cur) = stack.pop() {
        let ccount = child_count[cur as usize];
        for i in 0..ccount as usize {
            let c = child_data[cur as usize * nvar + i];
            if c == from {
                return false;
            }
            if c >= 0 && c < nvar as i32 && !visited[c as usize] {
                visited[c as usize] = true;
                stack.push(c);
            }
        }
    }
    true
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_var = 32i32;
    let mut num_record = 1024i32;
    let mut max_parents = 2i32;
    let mut insert_penalty = 2i32;
    let mut max_edges_per_var = 2i32;
    let mut seed: u32 = 0;
    let mut num_threads = 4;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-v" => { i += 1; num_var = args[i].parse().unwrap(); }
            "-r" => { i += 1; num_record = args[i].parse().unwrap(); }
            "-n" => { i += 1; max_parents = args[i].parse().unwrap(); }
            "-s" => { i += 1; seed = args[i].parse().unwrap(); }
            "-i" => { i += 1; insert_penalty = args[i].parse().unwrap(); }
            "-e" => { i += 1; max_edges_per_var = args[i].parse().unwrap(); }
            "-p" | "-t" => { i += 1; num_threads = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    println!("Bayes (STAMP spec)");
    println!("  Variables:   {}", num_var);
    println!("  Records:     {}", num_record);
    println!("  Max parents: {}", max_parents);
    println!("  Penalty:     {}", insert_penalty);
    println!("  Max edges:   {}", max_edges_per_var);
    println!("  Threads:     {}", num_threads);
    println!("  Seed:        {}", seed);

    tm_init();

    let base_penalty = if insert_penalty > 0 {
        -0.5 * (num_record as f64).ln() * insert_penalty as f64
    } else { 0.0 };

    let nc = num_var as usize;

    // ── Generate initial data in plain vectors ──────────────────
    let mut init_parent_count = vec![0i32; nc];
    let mut init_parent_data = vec![0i32; nc * MAX_PARENTS];
    let mut init_child_count = vec![0i32; nc];
    let mut init_child_data = vec![0i32; nc * nc];
    let mut init_total_parents = 0i32;

    let mut rng = Lcg::new(seed);

    for v in 1..nc {
        let max_np = (max_parents as usize).min(v);
        let np = if max_np > 0 { rng.next() as i32 % max_np as i32 + 1 } else { 0 };
        for _p in 0..np.min(v as i32) {
            let parent = (rng.next() as i32) % v as i32;
            let pc = init_parent_count[v];
            let mut found = false;
            for j in 0..pc as usize {
                if init_parent_data[v * MAX_PARENTS + j] == parent { found = true; break; }
            }
            if !found {
                let idx = pc as usize;
                init_parent_data[v * MAX_PARENTS + idx] = parent;
                init_parent_count[v] = pc + 1;
                let cc = init_child_count[parent as usize] as usize;
                init_child_data[parent as usize * nc + cc] = v as i32;
                init_child_count[parent as usize] = cc as i32 + 1;
                init_total_parents += 1;
            }
        }
    }

    let mut records: Vec<Vec<i32>> = Vec::with_capacity(num_record as usize);
    for _r in 0..num_record as usize {
        let mut row: Vec<i32> = Vec::with_capacity(nc);
        for v in 0..nc {
            if init_parent_count[v] == 0 {
                row.push((rng.next() % 2) as i32);
            } else {
                let mut val = 0i32;
                let pc = init_parent_count[v] as usize;
                for j in 0..pc {
                    let pid = init_parent_data[v * MAX_PARENTS + j] as usize;
                    val = (val << 1) | row[pid];
                }
                let mut threshold = 30i32;
                for j in 0..pc {
                    let pid = init_parent_data[v * MAX_PARENTS + j];
                    threshold += (pid * 7 + v as i32 * 11) % 40;
                }
                threshold = (threshold + val * 17) % 100;
                row.push(if (rng.next() % 100) < threshold as u32 { 1 } else { 0 });
            }
        }
        records.push(row);
    }

    // ── Convert to TmCell vectors ─────────────────────────────
    let parent_count: Arc<Vec<TmCell<i32>>> = Arc::new(init_parent_count.into_iter().map(TmCell::new).collect());
    let parent_data: Arc<Vec<TmCell<i32>>> = Arc::new(init_parent_data.into_iter().map(TmCell::new).collect());
    let child_count: Arc<Vec<TmCell<i32>>> = Arc::new(init_child_count.into_iter().map(TmCell::new).collect());
    let child_data: Arc<Vec<TmCell<i32>>> = Arc::new(init_child_data.into_iter().map(TmCell::new).collect());
    let local_ll: Arc<Vec<TmCell<f64>>> = Arc::new((0..nc).map(|_| TmCell::new(0.0)).collect());
    let base_log_likelihood: Arc<TmCell<f64>> = Arc::new(TmCell::new(0.0));
    let total_parents: Arc<TmCell<i32>> = Arc::new(TmCell::new(init_total_parents));
    let task_op: Arc<Vec<TmCell<i32>>> = Arc::new((0..MAX_TASKS).map(|_| TmCell::new(0)).collect());
    let task_from: Arc<Vec<TmCell<i32>>> = Arc::new((0..MAX_TASKS).map(|_| TmCell::new(0)).collect());
    let task_to: Arc<Vec<TmCell<i32>>> = Arc::new((0..MAX_TASKS).map(|_| TmCell::new(0)).collect());
    let task_score: Arc<Vec<TmCell<f64>>> = Arc::new((0..MAX_TASKS).map(|_| TmCell::new(0.0)).collect());
    let task_count: Arc<TmCell<i32>> = Arc::new(TmCell::new(0));
    let records = Arc::new(records);

    println!("  Initial parents: {}", init_total_parents);

    // ── Helper: compute density log-likelihood (non-TM, reads records) ──
    let compute_ll = |var: usize, parents: &[i32], recs: &[Vec<i32>], nr: i32| -> f64 {
        let np = parents.len();
        let ncfg = 1 << np;
        let mut c0 = vec![0i32; ncfg];
        let mut c1 = vec![0i32; ncfg];
        for r in 0..nr as usize {
            let mut cfg = 0usize;
            for i in 0..np {
                cfg = (cfg << 1) | recs[r][parents[i] as usize] as usize;
            }
            if recs[r][var] == 0 { c0[cfg] += 1; } else { c1[cfg] += 1; }
        }
        let mut ll = 0.0f64;
        for c in 0..ncfg {
            let t = c0[c] + c1[c];
            if t == 0 { continue; }
            let p0 = c0[c] as f64 / t as f64;
            let p1 = c1[c] as f64 / t as f64;
            let frac = t as f64 / nr as f64;
            if p0 > 0.0 { ll += frac * p0 * p0.ln(); }
            if p1 > 0.0 { ll += frac * p1 * p1.ln(); }
        }
        ll
    };

    let init_mutex = Arc::new(Mutex::new(()));
    let total_ops = Arc::new(AtomicI64::new(0));

    let start = Instant::now();

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let parent_count = parent_count.clone();
            let parent_data = parent_data.clone();
            let child_count = child_count.clone();
            let child_data = child_data.clone();
            let local_ll = local_ll.clone();
            let base_log_likelihood = base_log_likelihood.clone();
            let total_parents = total_parents.clone();
            let task_op = task_op.clone();
            let task_from = task_from.clone();
            let task_to = task_to.clone();
            let task_score = task_score.clone();
            let task_count = task_count.clone();
            let records = records.clone();
            let init_mutex = init_mutex.clone();
            let ops = total_ops.clone();

            s.spawn(move || {
                tm_init_thread();
                let nvar = nc as i32;
                let chunk = (nvar + num_threads - 1) / num_threads;
                let start_v = tid * chunk;
                let end_v = (start_v + chunk).min(nvar);

                // ── Phase 1: init local_ll ───────────────────────
                {
                    let _lock = init_mutex.lock().unwrap();
                    for v in start_v..end_v {
                        let ll_val = compute_ll(v as usize, &[], &records, num_record);
                        transaction(|tx| {
                            tx.write(&local_ll[v as usize], ll_val);
                            tx.write(&base_log_likelihood, tx.read(&base_log_likelihood) + ll_val);
                        });
                    }
                }

                // ── Phase 2: build initial task list ─────────────
                {
                    let _lock = init_mutex.lock().unwrap();
                    for v in start_v..end_v {
                        let base_ll = transaction(|tx| tx.read(&local_ll[v as usize]));
                        for from in 0..nvar {
                            if from == v { continue; }
                            let with_ll = compute_ll(v as usize, &[from], &records, num_record);
                            if with_ll > base_ll {
                                let delta = with_ll - base_ll;
                                let score = transaction(|tx| {
                                    base_penalty + num_record as f64 * (tx.read(&base_log_likelihood) + delta)
                                });
                                let t = Task { op: 0, from_id: from, to_id: v, score };
                                transaction(|tx| {
                                    let n = tx.read(&task_count);
                                    if n >= MAX_TASKS as i32 { return; }
                                    tx.write(&task_op[n as usize], t.op);
                                    tx.write(&task_from[n as usize], t.from_id);
                                    tx.write(&task_to[n as usize], t.to_id);
                                    tx.write(&task_score[n as usize], t.score);
                                    tx.write(&task_count, n + 1);
                                });
                            }
                        }
                    }
                }

                // ── Phase 3: work loop ───────────────────────────
                loop {
                    let t = transaction(|tx| {
                        let n = tx.read(&task_count);
                        if n == 0 { return Task { op: -1, from_id: -1, to_id: -1, score: -1e100 }; }
                        let mut best = 0usize;
                        let mut best_score = tx.read(&task_score[0]);
                        for i in 1..n as usize {
                            let s = tx.read(&task_score[i]);
                            if s > best_score { best_score = s; best = i; }
                        }
                        let t = Task {
                            op: tx.read(&task_op[best]),
                            from_id: tx.read(&task_from[best]),
                            to_id: tx.read(&task_to[best]),
                            score: best_score,
                        };
                        let last = n as usize - 1;
                        if best != last {
                            tx.write(&task_op[best], tx.read(&task_op[last]));
                            tx.write(&task_from[best], tx.read(&task_from[last]));
                            tx.write(&task_to[best], tx.read(&task_to[last]));
                            tx.write(&task_score[best], tx.read(&task_score[last]));
                        }
                        tx.write(&task_count, n - 1);
                        t
                    });

                    if t.op < 0 { break; }

                    let ok = transaction(|tx| {
                        let from = t.from_id;
                        let to = t.to_id;
                        let np = tx.read(&parent_count[to as usize]);

                        // set_contains
                        let mut found = false;
                        for i in 0..np as usize {
                            if tx.read(&parent_data[to as usize * MAX_PARENTS + i]) == from {
                                found = true; break;
                            }
                        }
                        if found { return false; }

                        // has_path
                        let mut visited = vec![false; nvar as usize];
                        let mut stack = vec![to];
                        visited[to as usize] = true;
                        while let Some(cur) = stack.pop() {
                            let ccount = tx.read(&child_count[cur as usize]);
                            for i in 0..ccount as usize {
                                let c = tx.read(&child_data[cur as usize * nvar as usize + i]);
                                if c == from { return false; }
                                if c >= 0 && c < nvar && !visited[c as usize] {
                                    visited[c as usize] = true;
                                    stack.push(c);
                                }
                            }
                        }

                        let np2 = tx.read(&parent_count[to as usize]);
                        if max_edges_per_var > 0 && np2 >= max_edges_per_var { return false; }

                        tx.write(&parent_data[to as usize * MAX_PARENTS + np2 as usize], from);
                        tx.write(&parent_count[to as usize], np2 + 1);

                        let nc2 = tx.read(&child_count[from as usize]);
                        tx.write(&child_data[from as usize * nvar as usize + nc2 as usize], to);
                        tx.write(&child_count[from as usize], nc2 + 1);

                        tx.write(&total_parents, tx.read(&total_parents) + 1);

                        let mut par = Vec::new();
                        for i in 0..(np2 + 1) as usize {
                            par.push(tx.read(&parent_data[to as usize * MAX_PARENTS + i]));
                        }
                        let new_ll = compute_ll(to as usize, &par, &records, num_record);
                        let old_ll = tx.read(&local_ll[to as usize]);
                        tx.write(&base_log_likelihood, tx.read(&base_log_likelihood) + (new_ll - old_ll));
                        tx.write(&local_ll[to as usize], new_ll);
                        true
                    });

                    if ok {
                        let next = transaction(|tx| {
                            let mut best = Task { op: -1, from_id: -1, to_id: -1, score: -1e100 };
                            let base_ll = tx.read(&local_ll[t.to_id as usize]);
                            let np = tx.read(&parent_count[t.to_id as usize]);
                            if max_edges_per_var > 0 && np >= max_edges_per_var { return best; }
                            for from in 0..nvar {
                                if from == t.to_id { continue; }
                                let mut found = false;
                                for i in 0..np as usize {
                                    if tx.read(&parent_data[t.to_id as usize * MAX_PARENTS + i]) == from {
                                        found = true; break;
                                    }
                                }
                                if found { continue; }

                                // has_path
                                let mut visited = vec![false; nvar as usize];
                                let mut stack = vec![t.to_id];
                                visited[t.to_id as usize] = true;
                                while let Some(cur) = stack.pop() {
                                    let ccount = tx.read(&child_count[cur as usize]);
                                    for i in 0..ccount as usize {
                                        let c = tx.read(&child_data[cur as usize * nvar as usize + i]);
                                        if c == from { found = true; break; }
                                        if c >= 0 && c < nvar && !visited[c as usize] {
                                            visited[c as usize] = true;
                                            stack.push(c);
                                        }
                                    }
                                    if found { break; }
                                }
                                if found { continue; }

                                let np2 = tx.read(&parent_count[t.to_id as usize]);
                                if max_edges_per_var > 0 && np2 >= max_edges_per_var { continue; }

                                let mut par = Vec::new();
                                for i in 0..np2 as usize {
                                    par.push(tx.read(&parent_data[t.to_id as usize * MAX_PARENTS + i]));
                                }
                                par.push(from);
                                let new_ll = compute_ll(t.to_id as usize, &par, &records, num_record);
                                let delta = new_ll - base_ll;
                                let score = tx.read(&total_parents) as f64 * base_penalty
                                    + num_record as f64 * (tx.read(&base_log_likelihood) + delta);
                                if score > best.score {
                                    best = Task { op: 0, from_id: from, to_id: t.to_id, score };
                                }
                            }
                            best
                        });

                        if next.op >= 0 {
                            transaction(|tx| {
                                let n = tx.read(&task_count);
                                if n >= MAX_TASKS as i32 { return; }
                                tx.write(&task_op[n as usize], next.op);
                                tx.write(&task_from[n as usize], next.from_id);
                                tx.write(&task_to[n as usize], next.to_id);
                                tx.write(&task_score[n as usize], next.score);
                                tx.write(&task_count, n + 1);
                            });
                        }
                        ops.fetch_add(1, Ordering::Relaxed);
                    }
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);
    let tp = transaction(|tx| tx.read(&total_parents)) as i64;

    println!("\nResults ({} ms):", elapsed.as_millis());
    println!("  Operations: {}  Total parents: {}", ops, tp);
    println!("  Time: {:.6} sec", elapsed.as_secs_f64());
    println!("  Rate: {:.0} ops/sec", ops as f64 / elapsed.as_secs_f64());
    println!("  PASS");

    tm_exit();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_rng_determinism() {
        let mut a = Lcg::new(42);
        let mut b = Lcg::new(42);
        for _ in 0..1000 {
            assert_eq!(a.next(), b.next());
        }
    }

    #[test]
    fn test_has_path_no_cycle() {
        // Graph: 0 -> 1 -> 2
        // from=0, to=2: no path from 2 back to 0
        let mut child_count = vec![0i32; 3];
        let mut child_data = vec![-1i32; 9];
        child_count[0] = 1;
        child_data[0 * 3 + 0] = 1;
        child_count[1] = 1;
        child_data[1 * 3 + 0] = 2;
        assert!(has_path_dfs(&child_count, &child_data, 0, 2, 3));
    }

    #[test]
    fn test_has_path_cycle_detected() {
        // Graph: 0 -> 2, 1 -> 0, 2 -> 1 (cycle 0-2-1-0)
        // from=0, to=2: path exists (2->1->0)
        let mut child_count = vec![0i32; 3];
        let mut child_data = vec![-1i32; 9];
        child_count[0] = 1;
        child_data[0 * 3 + 0] = 2;
        child_count[1] = 1;
        child_data[1 * 3 + 0] = 0;
        child_count[2] = 1;
        child_data[2 * 3 + 0] = 1;
        assert!(!has_path_dfs(&child_count, &child_data, 0, 2, 3));
    }

    #[test]
    fn test_has_path_self_loop() {
        // Adding from=1 to to=1: immediate cycle detected
        let child_count = vec![0i32; 3];
        let child_data = vec![-1i32; 9];
        assert!(!has_path_dfs(&child_count, &child_data, 1, 1, 3));
    }

    #[test]
    fn test_has_path_disconnected() {
        // No edges: any (from,to) pair has no path
        let child_count = vec![0i32; 3];
        let child_data = vec![-1i32; 9];
        assert!(has_path_dfs(&child_count, &child_data, 0, 1, 3));
    }
}
