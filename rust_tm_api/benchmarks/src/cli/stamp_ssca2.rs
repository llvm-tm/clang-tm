use std::collections::HashSet;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tm::*;
use benchmarks::Rng;

struct Edge {
    src: u64,
    dst: u64,
    weight: i64,
}

struct SparseRow {
    row_ptr: Vec<u64>,
    col_idx: Vec<u64>,
    #[allow(dead_code)]
    weights: Vec<i64>,
}

fn build_csr(edges: &mut Vec<Edge>) -> (SparseRow, u64) {
    if edges.is_empty() {
        return (SparseRow { row_ptr: vec![0], col_idx: vec![], weights: vec![] }, 0);
    }

    edges.sort_by(|a, b| {
        if a.src != b.src { a.src.cmp(&b.src) }
        else { a.dst.cmp(&b.dst) }
    });

    // Dedup
    edges.dedup_by(|a, b| a.src == b.src && a.dst == b.dst);

    let max_v = edges.iter()
        .map(|e| std::cmp::max(e.src, e.dst))
        .max()
        .unwrap_or(0);
    let num_vertices = max_v + 1;

    let mut row_ptr = vec![0u64; (num_vertices + 1) as usize];
    for e in edges.iter() {
        row_ptr[(e.src + 1) as usize] += 1;
    }
    for i in 1..=num_vertices as usize {
        row_ptr[i] += row_ptr[i - 1];
    }

    let num_edges = edges.len();
    let mut col_idx = vec![0u64; num_edges];
    let mut weights = vec![0i64; num_edges];
    let mut temp_pos = row_ptr.clone();

    for e in edges.iter() {
        let pos = temp_pos[e.src as usize] as usize;
        temp_pos[e.src as usize] += 1;
        col_idx[pos] = e.dst;
        weights[pos] = e.weight;
    }

    (SparseRow { row_ptr, col_idx, weights }, num_vertices)
}

fn ssca2_has_edge(row_ptr: &[u64], col_idx: &[u64], src: u64, dst: u64) -> bool {
    transaction(|_tx| {
        let start = row_ptr[src as usize];
        let end = row_ptr[(src + 1) as usize];
        for i in start..end {
            if col_idx[i as usize] == dst {
                return true;
            }
        }
        false
    })
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_threads = 4;
    let mut scale = 13;
    let mut prob_unidirectional = 0.5;
    let mut _subgr_edge_length = 3;
    let mut max_paral_edges = 3;
    let mut iterations = 3;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-s" => { i += 1; scale = args[i].parse().unwrap(); }
            "-u" => { i += 1; prob_unidirectional = args[i].parse().unwrap(); }
            "-l" => { i += 1; _subgr_edge_length = args[i].parse().unwrap(); }
            "-m" => { i += 1; max_paral_edges = args[i].parse().unwrap(); }
            "-i" => { i += 1; iterations = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    tm_init();

    let tot_vertices = 1u64 << scale;
    let max_clique_size = 1 << (scale / 3);
    let perc_int_weights = 0.6;
    let prob_intercl_edges = 0.5;

    let mut rng = Rng::new(42);

    // Random permutation
    let mut perm: Vec<u64> = (0..tot_vertices).collect();
    for i in (1..tot_vertices as usize).rev() {
        let j = (rng.next() as usize) % (i + 1);
        perm.swap(i, j);
    }

    // Generate clique sizes
    let mut clique_sizes: Vec<usize> = Vec::new();
    let mut assigned = 0u64;
    while assigned < tot_vertices {
        let sz = (rng.next() as u64 % max_clique_size as u64) + 1;
        let sz = std::cmp::min(sz, tot_vertices - assigned);
        clique_sizes.push(sz as usize);
        assigned += sz;
    }

    let mut edge_set: HashSet<(u64, u64)> = HashSet::new();
    let mut temp_edges: Vec<Edge> = Vec::new();

    // Intra-clique edges
    let mut start_v = 0u64;
    for &csize in &clique_sizes {
        for i in 0..csize {
            for j in 0..csize {
                if i == j { continue; }
                let u = rng.uniform();
                if u >= prob_unidirectional {
                    let si = perm[(start_v + i as u64) as usize];
                    let sj = perm[(start_v + j as u64) as usize];
                    if edge_set.insert((si, sj)) {
                        let wu = rng.uniform();
                        let w = if wu < perc_int_weights {
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
    for &csize in &clique_sizes {
        for i in 0..csize {
            let v = perm[(start_v + i as u64) as usize];
            let mut d = 1u64;
            while d < tot_vertices {
                let u = rng.uniform();
                let prob = prob_intercl_edges / ((d as f64).log2() + 1.0);
                if u < prob {
                    let neighbor = (v + d) % tot_vertices;
                    for _ in 0..max_paral_edges {
                        let u2 = rng.uniform();
                        if u2 < 0.5 {
                            if edge_set.insert((v, neighbor)) {
                                let wu = rng.uniform();
                                let w = if wu < perc_int_weights {
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

    // Finalize edges
    for e in &mut temp_edges {
        e.src = perm[e.src as usize % perm.len()];
        e.dst = perm[e.dst as usize % perm.len()];
    }

    let (graph, num_vertices) = build_csr(&mut temp_edges);
    let num_edges = graph.col_idx.len();

    println!("Number of processors:       {}", num_threads);
    println!("Problem Scale:              {}", scale);
    println!("Max parallel edges:         {}", max_paral_edges);
    println!("Percent int weights:        {}", perc_int_weights);
    println!("Probability unidirectional: {}", prob_unidirectional);
    println!("Vertices: {}  Edges: {}", num_vertices, num_edges);

    let graph = Arc::new(graph);
    let total_ops = Arc::new(AtomicU64::new(0));

    let start = Instant::now();

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let graph = graph.clone();
            let total_ops = total_ops.clone();

            s.spawn(move || {
                tm_init_thread();

                let chunk = (num_vertices as usize + num_threads - 1) / num_threads;
                let start_v = tid * chunk;
                let end_v = std::cmp::min(start_v + chunk, num_vertices as usize);

                let mut local_ops = 0u64;

                for _iter in 0..iterations {
                    for v in start_v..end_v {
                        let start = graph.row_ptr[v] as usize;
                        let end = graph.row_ptr[v + 1] as usize;

                        for i in start..end {
                            let neighbor = graph.col_idx[i];
                            let nstart = graph.row_ptr[neighbor as usize] as usize;
                            let nend = graph.row_ptr[(neighbor + 1) as usize] as usize;

                            for j in nstart..nend {
                                let n2 = graph.col_idx[j];
                                let triangle = ssca2_has_edge(&graph.row_ptr, &graph.col_idx, n2, v as u64);
                                if triangle {
                                    local_ops += 1;
                                }
                            }
                        }
                    }
                }

                total_ops.fetch_add(local_ops, Ordering::Relaxed);

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);

    println!("    Time = {} ms", elapsed.as_millis());
    println!("    Triangles = {}", ops);

    tm_exit();
}
