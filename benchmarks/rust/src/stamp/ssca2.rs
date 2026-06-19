use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

#[allow(dead_code)]
struct Edge { src: u64, dst: u64, weight: i64 }

#[allow(dead_code)]
struct CSR {
    row_ptr: Vec<TmCell<u64>>,
    col_idx: Vec<TmCell<u64>>,
    num_vertices: u64,
}

fn has_edge(csr: &CSR, src: u64, dst: u64) -> bool {
    transaction(|tx| {
        let start = tx.read(&csr.row_ptr[src as usize]);
        let end = tx.read(&csr.row_ptr[src as usize + 1]);
        for i in start..end {
            if tx.read(&csr.col_idx[i as usize]) == dst { return true; }
        }
        false
    })
}

pub fn test() -> i32 {
    let mut fails = 0;
    // Build simple graph CSR manually (no TM)
    let edges = [(0u64, 1u64), (1u64, 2u64), (2u64, 0u64), (1u64, 3u64)];
    let num_vertices = 4u64;
    let mut row_ptr = vec![0u64; num_vertices as usize + 1];
    for &(s, _) in &edges { row_ptr[s as usize + 1] += 1; }
    for i in 1..=num_vertices as usize { row_ptr[i] += row_ptr[i - 1]; }
    let mut col_idx = vec![0u64; edges.len()];
    let mut temp_pos = row_ptr.clone();
    for &(s, d) in &edges {
        let pos = temp_pos[s as usize] as usize;
        temp_pos[s as usize] += 1;
        col_idx[pos] = d;
    }
    // Check CSR structure
    if row_ptr.len() != 5 { eprintln!("FAIL: row_ptr len"); fails += 1; }
    if col_idx.len() != 4 { eprintln!("FAIL: col_idx len"); fails += 1; }
    // Check edges
    let has_edge = |s: u64, d: u64| -> bool {
        let start = row_ptr[s as usize];
        let end = row_ptr[s as usize + 1];
        (start..end).any(|i| col_idx[i as usize] == d)
    };
    if !has_edge(0, 1) { eprintln!("FAIL: missing edge 0->1"); fails += 1; }
    if !has_edge(1, 2) { eprintln!("FAIL: missing edge 1->2"); fails += 1; }
    if !has_edge(2, 0) { eprintln!("FAIL: missing edge 2->0"); fails += 1; }
    if !has_edge(1, 3) { eprintln!("FAIL: missing edge 1->3"); fails += 1; }
    if has_edge(0, 2) { eprintln!("FAIL: unexpected edge 0->2"); fails += 1; }
    // Check triangle (0,1,2)
    let mut found_triangle = false;
    for a in 0..num_vertices {
        for i in row_ptr[a as usize]..row_ptr[a as usize + 1] {
            let b = col_idx[i as usize];
            for j in row_ptr[b as usize]..row_ptr[b as usize + 1] {
                let c = col_idx[j as usize];
                if has_edge(c, a) { found_triangle = true; }
            }
        }
    }
    if !found_triangle { eprintln!("FAIL: triangle (0,1,2) not found"); fails += 1; }
    if fails > 0 { eprintln!("ssca2: {} test(s) failed", fails); }
    fails
}

pub fn run(config: &Config, _stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== SSCA2 ===");
    let scale = config.scale.max(2).min(20);
    let prob_unidirectional = 0.5;
    let max_paral_edges = 3usize;
    let subgr_edge_length = 3;
    let iterations = 10;
    println!("  Scale: {}  Vertices: {}  Prob unidirectional: {}  Max paral edges: {}  Subgr edge len: {}  Iterations: {}",
             scale, 1u64 << scale, prob_unidirectional, max_paral_edges, subgr_edge_length, iterations);

    let tot_vertices = 1u64 << scale;
    let max_clique_size = 1 << (scale / 3).max(1);
    let perc_int_weights = 0.6;
    let prob_intercl_edges = 0.5;

    let mut rng = Rng::new(42);

    // Permutation
    let mut perm: Vec<u64> = (0..tot_vertices).collect();
    for i in (1..tot_vertices as usize).rev() {
        let j = (rng.next() as usize) % (i + 1);
        perm.swap(i, j);
    }

    // Clique sizes
    let mut clique_sizes: Vec<usize> = Vec::new();
    let mut assigned = 0u64;
    while assigned < tot_vertices {
        let sz = ((rng.next() as usize) % max_clique_size + 1).min((tot_vertices - assigned) as usize);
        clique_sizes.push(sz);
        assigned += sz as u64;
    }

    // Generate edges
    let mut edge_set: HashSet<(u64, u64)> = HashSet::new();
    let mut temp_edges: Vec<Edge> = Vec::new();
    let mut start_v = 0u64;

    // Intra-clique edges
    for c in 0..clique_sizes.len() {
        let csize = clique_sizes[c];
        for i in 0..csize {
            for j in 0..csize {
                if i == j { continue; }
                if rng.uniform() < prob_unidirectional {
                    let si = perm[(start_v + i as u64) as usize];
                    let sj = perm[(start_v + j as u64) as usize];
                    if edge_set.insert((si, sj)) {
                        let w = if rng.uniform() < perc_int_weights {
                            (rng.next() % (1u64 << scale)) as i64
                        } else {
                            -(rng.next() as i64 % scale as i64)
                        };
                        temp_edges.push(Edge { src: si, dst: sj, weight: w });
                    }
                }
            }
        }
        start_v += csize as u64;
    }

    // Inter-clique edges
    start_v = 0;
    for c in 0..clique_sizes.len() {
        let csize = clique_sizes[c];
        for i in 0..csize {
            let v = perm[(start_v + i as u64) as usize];
            let mut d = 1u64;
            while d < tot_vertices {
                if rng.uniform() < prob_intercl_edges / ((d as f64).log2() + 1.0) {
                    let neighbor = (v + d) % tot_vertices;
                    for _ in 0..max_paral_edges {
                        if rng.uniform() < 0.5 {
                            if edge_set.insert((v, neighbor)) {
                                let w = if rng.uniform() < perc_int_weights {
                                    (rng.next() % (1u64 << scale)) as i64
                                } else {
                                    -(rng.next() as i64 % scale as i64)
                                };
                                temp_edges.push(Edge { src: v, dst: neighbor, weight: w });
                            }
                        }
                    }
                }
                d *= 2;
            }
        }
        start_v += csize as u64;
    }

    // Sort and dedup edges
    temp_edges.sort_by(|a, b| a.src.cmp(&b.src).then(a.dst.cmp(&b.dst)));
    temp_edges.dedup_by(|a, b| a.src == b.src && a.dst == b.dst);

    let num_edges = temp_edges.len();
    let max_v = temp_edges.iter().map(|e| e.src.max(e.dst)).max().unwrap_or(0);
    let num_vertices = max_v + 1;

    // Build CSR (plain Vec, no TM — matches C++ build_csr which uses tm_calloc but
    // then transitions to TM reads in worker)
    let mut row_ptr = vec![0u64; num_vertices as usize + 1];
    for e in &temp_edges {
        row_ptr[e.src as usize + 1] += 1;
    }
    for i in 1..=num_vertices as usize {
        row_ptr[i] += row_ptr[i - 1];
    }
    let mut col_idx = vec![0u64; num_edges];
    let mut temp_pos = row_ptr.clone();
    for e in &temp_edges {
        let pos = temp_pos[e.src as usize] as usize;
        temp_pos[e.src as usize] += 1;
        col_idx[pos] = e.dst;
    }

    // Wrap in TmCell for TM reads during triangle counting
    let csr = Arc::new(CSR {
        row_ptr: row_ptr.into_iter().map(TmCell::new).collect(),
        col_idx: col_idx.into_iter().map(TmCell::new).collect(),
        num_vertices,
    });

    println!("  Vertices: {}  Edges: {}", num_vertices, num_edges);

    let g_ops = AtomicU64::new(0);
    let t0 = std::time::Instant::now();

    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let c = csr.clone();
            let go = &g_ops;
            s.spawn(move || {
                let chunk = (num_vertices + config.threads as u64 - 1) / config.threads as u64;
                let start_v = tid as u64 * chunk;
                let end_v = (start_v + chunk).min(num_vertices);
                let mut local_ops = 0u64;

                for _iter in 0..iterations {
                    for v in start_v..end_v {
                        let s = c.row_ptr[v as usize].ptr();
                        let e = c.row_ptr[v as usize + 1].ptr();
                        let start = unsafe { *s };
                        let end = unsafe { *e };
                        for i in start..end {
                            let neighbor = unsafe { *c.col_idx[i as usize].ptr() };
                            let nstart = unsafe { *c.row_ptr[neighbor as usize].ptr() };
                            let nend = unsafe { *c.row_ptr[neighbor as usize + 1].ptr() };
                            for j in nstart..nend {
                                let n2 = unsafe { *c.col_idx[j as usize].ptr() };
                                if has_edge(&c, n2, v) {
                                    local_ops += 1;
                                }
                            }
                        }
                    }
                }

                go.fetch_add(local_ops, Ordering::Relaxed);
            });
        }
    });

    let elapsed = t0.elapsed().as_millis() as u64;
    let ops = g_ops.load(Ordering::Relaxed);
    println!("  Triangles: {}  Elapsed: {} ms  Rate: {} ops/s",
             ops, elapsed, if elapsed > 0 { ops * 1000 / elapsed } else { 0 });
}
