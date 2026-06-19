use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

const MAX_NEIGHBORS: usize = 16;

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct Edge { a: (i64, i64), b: (i64, i64) }

fn make_edge(ax: f64, ay: f64, bx: f64, by: f64) -> Edge {
    let abit = ax.to_bits();
    let bbit = bx.to_bits();
    if abit < bbit || (abit == bbit && ay.to_bits() < by.to_bits()) {
        Edge { a: (abit as i64, ay.to_bits() as i64), b: (bbit as i64, by.to_bits() as i64) }
    } else {
        Edge { a: (bbit as i64, by.to_bits() as i64), b: (abit as i64, ay.to_bits() as i64) }
    }
}

fn sq_dist(ax: f64, ay: f64, bx: f64, by: f64) -> f64 {
    let dx = ax - bx; let dy = ay - by; dx * dx + dy * dy
}

fn circumcircle(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> (f64, f64, f64) {
    let d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if d.abs() < 1e-15 { return (0.0, 0.0, 1e15); }
    let ccx = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
    let ccy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
    (ccx, ccy, sq_dist(ccx, ccy, ax, ay).sqrt())
}

fn tri_min_angle(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> f64 {
    let la = sq_dist(bx, by, cx, cy).sqrt();
    let lb = sq_dist(cx, cy, ax, ay).sqrt();
    let lc = sq_dist(ax, ay, bx, by).sqrt();
    let dot_a = ((bx - ax) * (cx - ax) + (by - ay) * (cy - ay)) / (lc * lb);
    let dot_b = ((cx - bx) * (ax - bx) + (cy - by) * (ay - by)) / (la * lc);
    let dot_c = ((ax - cx) * (bx - cx) + (ay - cy) * (by - cy)) / (lb * la);
    let aa = dot_a.clamp(-1.0, 1.0).acos() * 180.0 / std::f64::consts::PI;
    let ba = dot_b.clamp(-1.0, 1.0).acos() * 180.0 / std::f64::consts::PI;
    let ca = dot_c.clamp(-1.0, 1.0).acos() * 180.0 / std::f64::consts::PI;
    aa.min(ba).min(ca)
}

fn is_encroached(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> bool {
    let cex = (ax + bx) / 2.0; let cey = (ay + by) / 2.0;
    sq_dist(cex, cey, cx, cy) <= sq_dist(ax, ay, bx, by) / 4.0
}

struct YadaState {
    max_elems: usize,
    pt_x: Vec<TmCell<f64>>, pt_y: Vec<TmCell<f64>>,
    circ_x: Vec<TmCell<f64>>, circ_y: Vec<TmCell<f64>>, circ_r: Vec<TmCell<f64>>,
    min_angle: Vec<TmCell<f64>>,
    encroached: Vec<TmCell<i64>>, is_garbage: Vec<TmCell<i64>>,
    nbr_cnt: Vec<TmCell<i64>>, nbr_data: Vec<TmCell<i64>>,
    work_heap: Vec<TmCell<i64>>, work_heap_cnt: TmCell<i64>,
    elem_cnt: TmCell<i64>,
    angle: f64,
}

pub fn test() -> i32 {
    let mut fails = 0;
    // Test circumcircle of right triangle
    let (cx, cy, cr) = circumcircle(0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    if (cx - 0.5).abs() > 1e-10 { eprintln!("FAIL: circumcircle cx {}", cx); fails += 1; }
    if (cy - 0.5).abs() > 1e-10 { eprintln!("FAIL: circumcircle cy {}", cy); fails += 1; }
    let expected_r = (2.0f64).sqrt() / 2.0;
    if (cr - expected_r).abs() > 1e-10 { eprintln!("FAIL: circumcircle cr {}", cr); fails += 1; }
    // Test tri_min_angle of equilateral
    let ang = tri_min_angle(0.0, 0.0, 1.0, 0.0, 0.5, (3.0f64).sqrt() / 2.0);
    if (ang - 60.0).abs() > 1e-10 { eprintln!("FAIL: equilateral angle {}", ang); fails += 1; }
    // Test tri_min_angle of right
    let ang = tri_min_angle(0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
    if (ang - 45.0).abs() > 1e-10 { eprintln!("FAIL: right angle {}", ang); fails += 1; }
    // Test is_encroached
    if !is_encroached(0.0, 0.0, 2.0, 0.0, 1.0, 0.5) { eprintln!("FAIL: point inside edge circle"); fails += 1; }
    if is_encroached(0.0, 0.0, 2.0, 0.0, 1.0, 1.5) { eprintln!("FAIL: point outside edge circle"); fails += 1; }
    if fails > 0 { eprintln!("yada: {} test(s) failed", fails); }
    fails
}

pub fn run(config: &Config, stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== Yada ===");
    let grid_size: usize = config.points.max(10).min(30);
    let angle = config.dims as f64;
    let jitter = 0.5;
    println!("  Grid: {}×{}  Angle: {}°  Jitter: {:.2}", grid_size, grid_size, angle as i32, jitter);

    let spacing = 4.0;
    let mut rng = Rng::new(42);
    let n_grid = grid_size * grid_size;
    let mut gx = Vec::with_capacity(n_grid);
    let mut gy = Vec::with_capacity(n_grid);
    for i in 0..grid_size {
        for j in 0..grid_size {
            gx.push(i as f64 * spacing + (rng.uniform() - 0.5) * jitter * 2.0);
            gy.push(j as f64 * spacing + (rng.uniform() - 0.5) * jitter * 2.0);
        }
    }

    let max_elems = 200000;
    let mut pt_x = Vec::with_capacity(max_elems * 3);
    let mut pt_y = Vec::with_capacity(max_elems * 3);
    let mut circ_x = Vec::with_capacity(max_elems);
    let mut circ_y = Vec::with_capacity(max_elems);
    let mut circ_r = Vec::with_capacity(max_elems);
    let mut min_angle = Vec::with_capacity(max_elems);
    let mut encroached: Vec<TmCell<i64>> = Vec::with_capacity(max_elems);
    let mut is_garbage: Vec<TmCell<i64>> = Vec::with_capacity(max_elems);
    let mut nbr_cnt: Vec<TmCell<i64>> = Vec::with_capacity(max_elems);
    let mut nbr_data: Vec<TmCell<i64>> = Vec::with_capacity(max_elems * MAX_NEIGHBORS);
    let mut neighbors: Vec<Vec<usize>> = Vec::with_capacity(max_elems);

    let stride = grid_size;
    for i in 0..grid_size - 1 {
        for j in 0..grid_size - 1 {
            let i0 = i * stride + j; let i1 = (i + 1) * stride + j;
            let i2 = i * stride + (j + 1); let i3 = (i + 1) * stride + (j + 1);
            for triple in [(i0, i1, i3), (i0, i3, i2)] {
                let (ax, ay) = (gx[triple.0], gy[triple.0]);
                let (bx, by) = (gx[triple.1], gy[triple.1]);
                let (cx_, cy_) = (gx[triple.2], gy[triple.2]);
                let (ccx, ccy, cr) = circumcircle(ax, ay, bx, by, cx_, cy_);
                let ang = tri_min_angle(ax, ay, bx, by, cx_, cy_);
                pt_x.extend([ax, bx, cx_]); pt_y.extend([ay, by, cy_]);
                circ_x.push(ccx); circ_y.push(ccy); circ_r.push(cr);
                min_angle.push(ang);
                encroached.push(TmCell::new(0));
                is_garbage.push(TmCell::new(0));
                nbr_cnt.push(TmCell::new(0));
                neighbors.push(Vec::new());
            }
        }
    }
    let init_cnt = neighbors.len();

    for i in 0..init_cnt {
        for j in (i + 1)..init_cnt {
            let mut shared = 0;
            for vi in 0..3 {
                for vj in 0..3 {
                    if pt_x[i * 3 + vi].to_bits() == pt_x[j * 3 + vj].to_bits() &&
                       pt_y[i * 3 + vi].to_bits() == pt_y[j * 3 + vj].to_bits() { shared += 1; }
                }
            }
            if shared >= 2 { neighbors[i].push(j); neighbors[j].push(i); }
        }
    }

    let mut work_heap_init = Vec::new();
    for i in 0..init_cnt {
        if min_angle[i] < angle { work_heap_init.push(i as i64); }
    }
    println!("  Initial elements: {}  Bad: {}", init_cnt, work_heap_init.len());

    for i in 0..init_cnt {
        unsafe { *nbr_cnt[i].ptr() = neighbors[i].len() as i64; }
        for &n in &neighbors[i] { nbr_data.push(TmCell::new(n as i64)); }
        for _ in neighbors[i].len()..MAX_NEIGHBORS { nbr_data.push(TmCell::new(-1)); }
    }

    // Pad to max_elems
    for _ in init_cnt..max_elems {
        for _ in 0..3 { pt_x.push(0.0); pt_y.push(0.0); }
        circ_x.push(0.0); circ_y.push(0.0); circ_r.push(0.0); min_angle.push(180.0);
        encroached.push(TmCell::new(0)); is_garbage.push(TmCell::new(1));
        nbr_cnt.push(TmCell::new(0));
        for _ in 0..MAX_NEIGHBORS { nbr_data.push(TmCell::new(-1)); }
    }

    let wh_count = work_heap_init.len();
    let work_heap: Vec<TmCell<i64>> = (0..max_elems)
        .map(|i| TmCell::new(if i < wh_count { work_heap_init[i] } else { -1 }))
        .collect();

    let state = Arc::new(YadaState {
        max_elems, pt_x: pt_x.into_iter().map(TmCell::new).collect(),
        pt_y: pt_y.into_iter().map(TmCell::new).collect(),
        circ_x: circ_x.into_iter().map(TmCell::new).collect(),
        circ_y: circ_y.into_iter().map(TmCell::new).collect(),
        circ_r: circ_r.into_iter().map(TmCell::new).collect(),
        min_angle: min_angle.into_iter().map(TmCell::new).collect(),
        encroached, is_garbage, nbr_cnt, nbr_data, work_heap,
        work_heap_cnt: TmCell::new(wh_count as i64),
        elem_cnt: TmCell::new(init_cnt as i64),
        angle,
    });

    let dur = std::time::Duration::from_millis(config.duration as u64);
    let g_ops = AtomicU64::new(0);
    let t0 = std::time::Instant::now();

    std::thread::scope(|s| {
        for _tid in 0..config.threads {
            let st = state.clone();
            let sc = stop;
            let go = &g_ops;
            s.spawn(move || {
                loop {
                    if sc.load(Ordering::Relaxed) { break; }

                    // TX 1: Pop best from work heap
                    let el_id = transaction(|tx| {
                        let n = tx.read(&st.work_heap_cnt) as usize;
                        if n == 0 { return -1i64; }
                        let mut best_i = 0usize;
                        let mut best_id = tx.read(&st.work_heap[0]);
                        let mut max_enc = tx.read(&st.encroached[best_id as usize]);
                        for i in 1..n {
                            let id = tx.read(&st.work_heap[i]);
                            let enc = tx.read(&st.encroached[id as usize]);
                            if enc > max_enc { max_enc = enc; best_i = i; best_id = id; }
                        }
                        let last = n - 1;
                        if best_i != last {
                            let tmp = tx.read(&st.work_heap[last]);
                            tx.write(&st.work_heap[best_i], tmp);
                        }
                        tx.write(&st.work_heap_cnt, last as i64);
                        best_id
                    });
                    if el_id < 0 { continue; }

                    // Check if already garbage
                    let garb = transaction(|tx| tx.read(&st.is_garbage[el_id as usize]));
                    if garb != 0 { continue; }

                    // TX 2: Refine element (BFS + write new elements inside TM)
                    let num_new = transaction(|tx| {
                        let eid = el_id as usize;
                        if tx.read(&st.is_garbage[eid]) != 0 { return 0i64; }
                        let p0 = (tx.read(&st.pt_x[eid * 3]), tx.read(&st.pt_y[eid * 3]));
                        let p1 = (tx.read(&st.pt_x[eid * 3 + 1]), tx.read(&st.pt_y[eid * 3 + 1]));
                        let p2 = (tx.read(&st.pt_x[eid * 3 + 2]), tx.read(&st.pt_y[eid * 3 + 2]));
                        let seed_cx = tx.read(&st.circ_x[eid]);
                        let seed_cy = tx.read(&st.circ_y[eid]);
                        let seed_cr = tx.read(&st.circ_r[eid]);
                        let seed_cr_sq = seed_cr * seed_cr + 1e-10;

                        // BFS cavity
                        let mut before: Vec<usize> = Vec::new();
                        let mut bfs: Vec<usize> = Vec::new();
                        let mut border: Vec<Edge> = Vec::new();
                        let mut visited: Vec<bool> = vec![false; tx.read(&st.elem_cnt) as usize];
                        before.push(eid); bfs.push(eid); visited[eid] = true;
                        let mut qidx = 0;

                        while qidx < bfs.len() {
                            let cur = bfs[qidx]; qidx += 1;
                            let cur_cx = tx.read(&st.circ_x[cur]);
                            let cur_cy = tx.read(&st.circ_y[cur]);
                            let d2 = sq_dist(cur_cx, cur_cy, seed_cx, seed_cy);
                            if d2 > seed_cr_sq {
                                for j in 0..3 {
                                    let ax = tx.read(&st.pt_x[cur * 3 + j]);
                                    let ay = tx.read(&st.pt_y[cur * 3 + j]);
                                    let bx = tx.read(&st.pt_x[cur * 3 + (j + 1) % 3]);
                                    let by = tx.read(&st.pt_y[cur * 3 + (j + 1) % 3]);
                                    border.push(make_edge(ax, ay, bx, by));
                                }
                                continue;
                            }
                            let nc = tx.read(&st.nbr_cnt[cur]) as usize;
                            for ni in 0..nc {
                                let nid = tx.read(&st.nbr_data[cur * MAX_NEIGHBORS + ni]) as usize;
                                if nid < visited.len() && !visited[nid] {
                                    visited[nid] = true;
                                    bfs.push(nid); before.push(nid);
                                }
                            }
                        }

                        // Mark cavity as garbage
                        for &b in &before { tx.write(&st.is_garbage[b], 1); }

                        // Centroid
                        let cx = p0.0 / 3.0 + p1.0 / 3.0 + p2.0 / 3.0;
                        let cy = p0.1 / 3.0 + p1.1 / 3.0 + p2.1 / 3.0;

                        // Dedup border edges
                        let mut dedup: Vec<Edge> = Vec::new();
                        for e in &border { if !dedup.contains(e) { dedup.push(*e); } }
                        if dedup.len() > 3 { dedup.truncate(3); }
                        if dedup.len() < 2 { return 0i64; }

                        let saved = tx.read(&st.elem_cnt) as usize;
                        let mut new_count = 0i64;

                        for edge in &dedup {
                            let tid = tx.read(&st.elem_cnt) as usize;
                            if tid >= st.max_elems { break; }
                            tx.write(&st.elem_cnt, (tid + 1) as i64);

                            let ea = (f64::from_bits(edge.a.0 as u64), f64::from_bits(edge.a.1 as u64));
                            let eb = (f64::from_bits(edge.b.0 as u64), f64::from_bits(edge.b.1 as u64));

                            tx.write(&st.pt_x[tid * 3], cx);
                            tx.write(&st.pt_y[tid * 3], cy);
                            tx.write(&st.pt_x[tid * 3 + 1], ea.0);
                            tx.write(&st.pt_y[tid * 3 + 1], ea.1);
                            tx.write(&st.pt_x[tid * 3 + 2], eb.0);
                            tx.write(&st.pt_y[tid * 3 + 2], eb.1);

                            let (ccx, ccy, cr) = circumcircle(cx, cy, ea.0, ea.1, eb.0, eb.1);
                            tx.write(&st.circ_x[tid], ccx);
                            tx.write(&st.circ_y[tid], ccy);
                            tx.write(&st.circ_r[tid], cr);
                            let ma = tri_min_angle(cx, cy, ea.0, ea.1, eb.0, eb.1);
                            tx.write(&st.min_angle[tid], ma);
                            tx.write(&st.encroached[tid], 0);
                            tx.write(&st.is_garbage[tid], 0);
                            tx.write(&st.nbr_cnt[tid], 0);

                            let enc = is_encroached(cx, cy, ea.0, ea.1, eb.0, eb.1) ||
                                      is_encroached(ea.0, ea.1, eb.0, eb.1, cx, cy) ||
                                      is_encroached(eb.0, eb.1, cx, cy, ea.0, ea.1);
                            if enc { tx.write(&st.encroached[tid], 1); }

                            if ma < st.angle || enc {
                                let wn = tx.read(&st.work_heap_cnt) as usize;
                                if wn < st.max_elems {
                                    tx.write(&st.work_heap[wn], tid as i64);
                                    tx.write(&st.work_heap_cnt, (wn + 1) as i64);
                                }
                            }
                            new_count += 1;
                        }

                        // Reuse seed element slot as stencil
                        if new_count > 0 && saved < st.max_elems {
                            tx.write(&st.pt_x[saved * 3], p0.0);
                            tx.write(&st.pt_y[saved * 3], p0.1);
                            tx.write(&st.pt_x[saved * 3 + 1], p1.0);
                            tx.write(&st.pt_y[saved * 3 + 1], p1.1);
                            tx.write(&st.pt_x[saved * 3 + 2], p2.0);
                            tx.write(&st.pt_y[saved * 3 + 2], p2.1);
                            let (ccx, ccy, cr) = circumcircle(cx, cy, p0.0, p0.1, p1.0, p1.1);
                            tx.write(&st.circ_x[saved], ccx);
                            tx.write(&st.circ_y[saved], ccy);
                            tx.write(&st.circ_r[saved], cr);
                            tx.write(&st.min_angle[saved], 180.0);
                            tx.write(&st.is_garbage[saved], 0);
                            tx.write(&st.nbr_cnt[saved], 0);
                        }

                        new_count
                    });

                    if num_new > 0 {
                        go.fetch_add(1, Ordering::Relaxed);
                    }
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });

    let elapsed = t0.elapsed().as_millis() as u64;
    let ops_count = g_ops.load(Ordering::Relaxed);
    let final_el = unsafe { *state.elem_cnt.ptr() };
    let mut garb = 0i64;
    for i in 0..final_el as usize { if unsafe { *state.is_garbage[i].ptr() } != 0 { garb += 1; } }
    println!("  Operations: {}  Total elements: {} (garbage: {})  Elapsed: {} ms",
             ops_count, final_el, garb, elapsed);
}
