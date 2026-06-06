use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tm::*;

const MAX_NEIGHBORS: usize = 16;
const MAX_ELEMENTS: usize = 200000;
const GRID_SIZE: usize = 10;
const SPACING: f64 = 4.0;
const PI: f64 = std::f64::consts::PI;

#[derive(Clone, Copy)]
struct Point { x: f64, y: f64 }

impl PartialEq for Point {
    fn eq(&self, other: &Self) -> bool { self.x == other.x && self.y == other.y }
}
impl Eq for Point {}
impl std::hash::Hash for Point {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        (self.x.to_bits()).hash(state);
        (self.y.to_bits()).hash(state);
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct Edge { a: Point, b: Point }

fn make_edge(a: Point, b: Point) -> Edge {
    if b.x < a.x || (b.x == a.x && b.y < a.y) {
        Edge { a: b, b: a }
    } else {
        Edge { a, b }
    }
}

fn dist2(a: Point, b: Point) -> f64 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    dx * dx + dy * dy
}

fn circumcircle(a: Point, b: Point, c: Point) -> (f64, f64, f64) {
    let d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if d.abs() < 1e-15 { return (0.0, 0.0, 1e15); }
    let cx = ((a.x * a.x + a.y * a.y) * (b.y - c.y)
        + (b.x * b.x + b.y * b.y) * (c.y - a.y)
        + (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d;
    let cy = ((a.x * a.x + a.y * a.y) * (c.x - b.x)
        + (b.x * b.x + b.y * b.y) * (a.x - c.x)
        + (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d;
    let cr = dist2(Point { x: cx, y: cy }, a).sqrt();
    (cx, cy, cr)
}

fn angle_at(p: Point, q: Point, r: Point) -> f64 {
    let d1 = dist2(p, q).sqrt();
    let d2 = dist2(p, r).sqrt();
    if d1 < 1e-15 || d2 < 1e-15 { return 180.0; }
    let dot = ((q.x - p.x) * (r.x - p.x) + (q.y - p.y) * (r.y - p.y)) / (d1 * d2);
    let dot = dot.clamp(-1.0, 1.0);
    dot.acos() * 180.0 / PI
}

fn tri_min_angle(a: Point, b: Point, c: Point) -> f64 {
    angle_at(a, b, c).min(angle_at(b, a, c)).min(angle_at(c, a, b))
}

fn is_encroached(edge_a: Point, edge_b: Point, c: Point) -> bool {
    let mx = (edge_a.x + edge_b.x) / 2.0;
    let my = (edge_a.y + edge_b.y) / 2.0;
    let r2 = dist2(edge_a, edge_b) / 4.0;
    dist2(Point { x: mx, y: my }, c) <= r2
}

// ── LCG matching yada C++ pattern ────────────────────────────────
struct Lcg(u32);
impl Lcg {
    fn new(seed: u32) -> Self { Lcg(if seed == 0 { 1 } else { seed }) }
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(1103515245).wrapping_add(12345);
        self.0 & 0x7fffffff
    }
}
fn uniform(rng: &mut Lcg) -> f64 { rng.next() as f64 / 2147483648.0 }

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut angle_constraint = 20.0;
    let mut jitter = 0.5;
    let mut num_threads = 4;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-a" => { i += 1; angle_constraint = args[i].parse().unwrap(); }
            "-j" => { i += 1; jitter = args[i].parse().unwrap(); }
            "-p" | "-t" => { i += 1; num_threads = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    println!("Yada (STAMP spec, Delaunay mesh refinement)");
    println!("  Angle constraint: {}°", angle_constraint);
    println!("  Jitter:           {:.2}", jitter);
    println!("  Threads:          {}", num_threads);

    tm_init();

    // ── Generate synthetic mesh ────────────────────────────────
    let mut rng = Lcg::new(42);
    let n = GRID_SIZE;
    let mut verts: Vec<Point> = Vec::new();
    for iy in 0..n {
        for ix in 0..n {
            let x = ix as f64 * SPACING + (uniform(&mut rng) - 0.5) * 2.0 * jitter;
            let y = iy as f64 * SPACING + (uniform(&mut rng) - 0.5) * 2.0 * jitter;
            verts.push(Point { x, y });
        }
    }

    let mut init_pts: Vec<(Point, Point, Point)> = Vec::new();
    for iy in 0..n - 1 {
        for ix in 0..n - 1 {
            let i0 = iy * n + ix;
            let i1 = iy * n + ix + 1;
            let i2 = (iy + 1) * n + ix;
            let i3 = (iy + 1) * n + ix + 1;
            let (p0, p1, p2, p3) = (verts[i0], verts[i1], verts[i2], verts[i3]);
            init_pts.push((p0, p1, p3));
            init_pts.push((p0, p3, p2));
        }
    }

    let nelem = init_pts.len();

    // ── Compute neighbors ───────────────────────────────────────
    let nbr_cnt_cell: std::cell::RefCell<Vec<i64>> = std::cell::RefCell::new(vec![0i64; nelem]);
    let nbr_data_cell: std::cell::RefCell<Vec<i64>> = std::cell::RefCell::new(vec![0i64; nelem * MAX_NEIGHBORS]);
    for i in 0..nelem {
        let (a1, b1, c1) = init_pts[i];
        for j in (i + 1)..nelem {
            let (a2, b2, c2) = init_pts[j];
            let share = [a1, b1, c1].iter().filter(|&p| *p == a2 || *p == b2 || *p == c2).count();
            if share == 2 {
                let mut cnt = nbr_cnt_cell.borrow_mut();
                let mut data = nbr_data_cell.borrow_mut();
                let ni = cnt[i] as usize;
                let nj = cnt[j] as usize;
                data[i * MAX_NEIGHBORS + ni] = j as i64;
                data[j * MAX_NEIGHBORS + nj] = i as i64;
                cnt[i] += 1;
                cnt[j] += 1;
            }
        }
    }
    let init_nbr_cnt = nbr_cnt_cell.into_inner();
    let init_nbr_data = nbr_data_cell.into_inner();

    // ── Compute circumcircles and min angles ────────────────────
    let init_circ_x: Vec<f64> = init_pts.iter().map(|&(a,b,c)| circumcircle(a,b,c).0).collect();
    let init_circ_y: Vec<f64> = init_pts.iter().map(|&(a,b,c)| circumcircle(a,b,c).1).collect();
    let init_circ_r: Vec<f64> = init_pts.iter().map(|&(a,b,c)| circumcircle(a,b,c).2).collect();
    let init_min_angle: Vec<f64> = init_pts.iter().map(|&(a,b,c)| tri_min_angle(a,b,c)).collect();
    let init_encroached: Vec<i64> = init_pts.iter().map(|&(a,b,c)| {
        let mut e = 0;
        if is_encroached(a, b, c) { e = 1; }
        if is_encroached(b, c, a) { e = 1; }
        if is_encroached(c, a, b) { e = 1; }
        e
    }).collect();

    // ── Allocate TM-tracked arrays ─────────────────────────────
    // All fields use TmCell so they can be read/written inside transaction closures.
    // This matches C++ where TM fields go through tm_calloc and geometry is plain heap;
    // but for Rust, making everything TmCell is simpler and correct.
    let encroached: Vec<TmCell<i64>> = (0..MAX_ELEMENTS).map(|_| TmCell::new(0)).collect();
    let is_garbage: Vec<TmCell<i64>> = (0..MAX_ELEMENTS).map(|_| TmCell::new(0)).collect();
    let is_referenced: Vec<TmCell<i64>> = (0..MAX_ELEMENTS).map(|_| TmCell::new(0)).collect();
    let nbr_cnt: Vec<TmCell<i64>> = (0..MAX_ELEMENTS).map(|i| TmCell::new(if i < nelem { init_nbr_cnt[i] } else { 0 })).collect();
    let nbr_data: Vec<TmCell<i64>> = (0..MAX_ELEMENTS * MAX_NEIGHBORS)
        .map(|i| TmCell::new(if i < init_nbr_data.len() { init_nbr_data[i] } else { 0 })).collect();
    let work_heap: Vec<TmCell<i64>> = (0..MAX_ELEMENTS).map(|_| TmCell::new(0)).collect();
    let work_heap_cnt = TmCell::new(0i64);
    let elem_count = TmCell::new(nelem as i64);

    // Geometry — also TmCell for mutation inside transactions
    let pt_x: Vec<TmCell<f64>> = (0..MAX_ELEMENTS * 3).map(|i| {
        TmCell::new(if i / 3 < nelem {
            let (a, b, c) = init_pts[i / 3];
            match i % 3 { 0 => a.x, 1 => b.x, 2 => c.x, _ => unreachable!() }
        } else { 0.0 })
    }).collect();
    let pt_y: Vec<TmCell<f64>> = (0..MAX_ELEMENTS * 3).map(|i| {
        TmCell::new(if i / 3 < nelem {
            let (a, b, c) = init_pts[i / 3];
            match i % 3 { 0 => a.y, 1 => b.y, 2 => c.y, _ => unreachable!() }
        } else { 0.0 })
    }).collect();
    let circ_x: Vec<TmCell<f64>> = (0..MAX_ELEMENTS)
        .map(|i| TmCell::new(if i < nelem { init_circ_x[i] } else { 0.0 })).collect();
    let circ_y: Vec<TmCell<f64>> = (0..MAX_ELEMENTS)
        .map(|i| TmCell::new(if i < nelem { init_circ_y[i] } else { 0.0 })).collect();
    let circ_r: Vec<TmCell<f64>> = (0..MAX_ELEMENTS)
        .map(|i| TmCell::new(if i < nelem { init_circ_r[i] } else { 0.0 })).collect();
    let min_angle: Vec<TmCell<f64>> = (0..MAX_ELEMENTS)
        .map(|i| TmCell::new(if i < nelem { init_min_angle[i] } else { 0.0 })).collect();

    // ── Initial work heap ───────────────────────────────────────
    let init_bad = transaction(|tx| {
        let mut bc = 0i64;
        for i in 0..nelem {
            if init_min_angle[i] < angle_constraint || init_encroached[i] != 0 {
                tx.write(&is_referenced[i], 1);
                tx.write(&work_heap[bc as usize], i as i64);
                bc += 1;
            }
        }
        tx.write(&work_heap_cnt, bc);
        bc
    });

    // Wrap everything in Arc for sharing
    let shared_alloc = SharedData {
        encroached: encroached.into(),
        is_garbage: is_garbage.into(),
        is_referenced: is_referenced.into(),
        nbr_cnt: nbr_cnt.into(),
        nbr_data: nbr_data.into(),
        work_heap: work_heap.into(),
        work_heap_cnt: work_heap_cnt.into(),
        elem_count: elem_count.into(),
        pt_x: pt_x.into(),
        pt_y: pt_y.into(),
        circ_x: circ_x.into(),
        circ_y: circ_y.into(),
        circ_r: circ_r.into(),
        min_angle: min_angle.into(),
    };
    eprintln!("  Initial elements: {}  Bad: {} (angle constraint={}°)",
              nelem, init_bad, angle_constraint);

    let stop = Arc::new(AtomicBool::new(false));
    let total_ops = Arc::new(AtomicI64::new(0));
    let start = Instant::now();

    // ── Worker threads ───────────────────────────────────────────
    std::thread::scope(|s| {
        for _tid in 0..num_threads {
            let d = shared_alloc.clone();
            let stop = &stop;
            let total_ops = &total_ops;
            let start = Instant::now();

            s.spawn(move || {
                tm_init_thread();
                let ac = angle_constraint;
                let maxel = MAX_ELEMENTS;

                while !stop.load(Ordering::Relaxed) {
                    // ── TX 1: Pop work ──────────────────────────
                    let el_id = transaction(|tx| {
                        let n = tx.read(&d.work_heap_cnt);
                        if n == 0 { return -1i64 }
                        let mut best_i = 0usize;
                        let mut best_id = tx.read(&d.work_heap[0]);
                        let mut max_enc = tx.read(&d.encroached[best_id as usize]);
                        for i in 1..n as usize {
                            let id = tx.read(&d.work_heap[i]);
                            let enc = tx.read(&d.encroached[id as usize]);
                            if enc > max_enc {
                                max_enc = enc;
                                best_i = i;
                                best_id = id;
                            }
                        }
                        let last = n as usize - 1;
                        if best_i != last {
                            let tmp = tx.read(&d.work_heap[last]);
                            tx.write(&d.work_heap[best_i], tmp);
                        }
                        tx.write(&d.work_heap_cnt, n - 1);
                        best_id
                    });

                    if el_id < 0 {
                        if Instant::now().duration_since(start).as_secs_f64() > 3.0 {
                            stop.store(true, Ordering::Relaxed);
                        }
                        continue;
                    }

                    let garb = transaction(|tx| tx.read(&d.is_garbage[el_id as usize]));
                    if garb != 0 {
                        if Instant::now().duration_since(start).as_secs_f64() > 3.0 {
                            stop.store(true, Ordering::Relaxed);
                        }
                        continue;
                    }

                    transaction(|tx| { tx.write(&d.is_referenced[el_id as usize], 0); });

                    let _old_count = transaction(|tx| tx.read(&d.elem_count));

                    // ── TX 2: Refine element ────────────────────
                    let (_saved_id, num_new, new_bad_ids) = transaction(|tx| {
                        let eid = el_id as usize;

                        let p0 = Point {
                            x: tx.read(&d.pt_x[eid * 3]),
                            y: tx.read(&d.pt_y[eid * 3]),
                        };
                        let p1 = Point {
                            x: tx.read(&d.pt_x[eid * 3 + 1]),
                            y: tx.read(&d.pt_y[eid * 3 + 1]),
                        };
                        let p2 = Point {
                            x: tx.read(&d.pt_x[eid * 3 + 2]),
                            y: tx.read(&d.pt_y[eid * 3 + 2]),
                        };
                        let seed_cx = tx.read(&d.circ_x[eid]);
                        let seed_cy = tx.read(&d.circ_y[eid]);
                        let seed_cr = tx.read(&d.circ_r[eid]);
                        let seed_cr_sq = seed_cr * seed_cr + 1e-10;

                        // BFS: grow cavity — use local Vecs inside the closure
                        let mut local_before: Vec<i64> = Vec::new();
                        let mut local_bfs: Vec<i64> = Vec::new();
                        let mut local_border: HashSet<Edge> = HashSet::new();
                        let mut local_bad: Vec<i64> = Vec::new();

                        local_before.push(el_id);
                        local_bfs.push(el_id);
                        let mut visited = HashSet::new();
                        visited.insert(el_id);
                        let mut qidx = 0usize;

                        while qidx < local_bfs.len() {
                            let cur = local_bfs[qidx] as usize;
                            qidx += 1;
                            let cur_cx = tx.read(&d.circ_x[cur]);
                            let cur_cy = tx.read(&d.circ_y[cur]);
                            let dx = cur_cx - seed_cx;
                            let dy = cur_cy - seed_cy;
                            let d2 = dx * dx + dy * dy;

                            if d2 > seed_cr_sq {
                                let ca = Point {
                                    x: tx.read(&d.pt_x[cur * 3]),
                                    y: tx.read(&d.pt_y[cur * 3]),
                                };
                                let cb = Point {
                                    x: tx.read(&d.pt_x[cur * 3 + 1]),
                                    y: tx.read(&d.pt_y[cur * 3 + 1]),
                                };
                                let cc = Point {
                                    x: tx.read(&d.pt_x[cur * 3 + 2]),
                                    y: tx.read(&d.pt_y[cur * 3 + 2]),
                                };
                                local_border.insert(make_edge(ca, cb));
                                local_border.insert(make_edge(cb, cc));
                                local_border.insert(make_edge(cc, ca));
                                continue;
                            }

                            let nc = tx.read(&d.nbr_cnt[cur]);
                            for ni in 0..nc as usize {
                                let nid = tx.read(&d.nbr_data[cur * MAX_NEIGHBORS + ni]);
                                if visited.insert(nid) {
                                    local_bfs.push(nid);
                                    local_before.push(nid);
                                }
                            }
                        }

                        for &bid in &local_before {
                            tx.write(&d.is_garbage[bid as usize], 1);
                        }

                        let cx = p0.x / 3.0 + p1.x / 3.0 + p2.x / 3.0;
                        let cy = p0.y / 3.0 + p1.y / 3.0 + p2.y / 3.0;
                        let centroid = Point { x: cx, y: cy };

                        let edges: Vec<Edge> = local_border.iter().take(3).copied().collect();
                        let saved = tx.read(&d.elem_count);
                        let mut new_count = 0i64;

                        for edge in &edges {
                            let tid = tx.read(&d.elem_count) as usize;
                            if tid >= maxel { break; }
                            tx.write(&d.elem_count, tid as i64 + 1);

                            tx.write(&d.pt_x[tid * 3], centroid.x);
                            tx.write(&d.pt_y[tid * 3], centroid.y);
                            tx.write(&d.pt_x[tid * 3 + 1], edge.a.x);
                            tx.write(&d.pt_y[tid * 3 + 1], edge.a.y);
                            tx.write(&d.pt_x[tid * 3 + 2], edge.b.x);
                            tx.write(&d.pt_y[tid * 3 + 2], edge.b.y);

                            let (cx_, cy_, cr_) = circumcircle(centroid, edge.a, edge.b);
                            tx.write(&d.circ_x[tid], cx_);
                            tx.write(&d.circ_y[tid], cy_);
                            tx.write(&d.circ_r[tid], cr_);
                            let ma = tri_min_angle(centroid, edge.a, edge.b);
                            tx.write(&d.min_angle[tid], ma);

                            tx.write(&d.encroached[tid], 0);
                            tx.write(&d.is_garbage[tid], 0);
                            tx.write(&d.is_referenced[tid], 0);

                            let mut enc = false;
                            if is_encroached(centroid, edge.a, edge.b) { enc = true; }
                            if is_encroached(edge.a, edge.b, centroid) { enc = true; }
                            if is_encroached(edge.b, centroid, edge.a) { enc = true; }
                            if enc { tx.write(&d.encroached[tid], 1); }

                            if ma < ac || enc {
                                if tx.read(&d.is_referenced[tid]) == 0 {
                                    tx.write(&d.is_referenced[tid], 1);
                                    local_bad.push(tid as i64);
                                }
                            }
                            new_count += 1;
                        }

                        // Reuse seed element slot
                        if new_count > 0 && (saved as usize) < maxel {
                            tx.write(&d.pt_x[saved as usize * 3], p0.x);
                            tx.write(&d.pt_y[saved as usize * 3], p0.y);
                            tx.write(&d.pt_x[saved as usize * 3 + 1], p1.x);
                            tx.write(&d.pt_y[saved as usize * 3 + 1], p1.y);
                            tx.write(&d.pt_x[saved as usize * 3 + 2], p2.x);
                            tx.write(&d.pt_y[saved as usize * 3 + 2], p2.y);

                            let (cx_, cy_, cr_) = circumcircle(centroid, p0, p1);
                            tx.write(&d.circ_x[saved as usize], cx_);
                            tx.write(&d.circ_y[saved as usize], cy_);
                            tx.write(&d.circ_r[saved as usize], cr_);
                            tx.write(&d.min_angle[saved as usize], 180.0);

                            tx.write(&d.is_garbage[saved as usize], 0);
                            tx.write(&d.is_referenced[saved as usize], 0);
                        }

                        (saved, new_count, local_bad)
                    });

                    if num_new > 0 {
                        // ── TX 3: Push new bad elements ──────────
                        transaction(|tx| {
                            for &bid in &new_bad_ids {
                                if tx.read(&d.is_referenced[bid as usize]) != 0 {
                                    let n = tx.read(&d.work_heap_cnt) as usize;
                                    if n < maxel {
                                        tx.write(&d.work_heap[n], bid);
                                        tx.write(&d.work_heap_cnt, n as i64 + 1);
                                    }
                                }
                            }
                        });
                        total_ops.fetch_add(1, Ordering::Relaxed);
                    }

                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);
    let final_ec = transaction(|tx| tx.read(&shared_alloc.elem_count));
    let final_garb = transaction(|tx| {
        let mut g = 0i64;
        for i in 0..final_ec as usize {
            if tx.read(&shared_alloc.is_garbage[i]) != 0 { g += 1; }
        }
        g
    });

    println!("\nResults ({} ms):", elapsed.as_millis());
    println!("  Operations: {}  Total elements: {} (garbage: {})",
             ops, final_ec, final_garb);
    println!("  Time: {:.6} sec", elapsed.as_secs_f64());
    println!("  Rate: {:.0} ops/sec", ops as f64 / elapsed.as_secs_f64());
    println!("  PASS");

    tm_exit();
}

struct SharedData {
    encroached: Arc<Vec<TmCell<i64>>>,
    is_garbage: Arc<Vec<TmCell<i64>>>,
    is_referenced: Arc<Vec<TmCell<i64>>>,
    nbr_cnt: Arc<Vec<TmCell<i64>>>,
    nbr_data: Arc<Vec<TmCell<i64>>>,
    work_heap: Arc<Vec<TmCell<i64>>>,
    work_heap_cnt: Arc<TmCell<i64>>,
    elem_count: Arc<TmCell<i64>>,
    pt_x: Arc<Vec<TmCell<f64>>>,
    pt_y: Arc<Vec<TmCell<f64>>>,
    circ_x: Arc<Vec<TmCell<f64>>>,
    circ_y: Arc<Vec<TmCell<f64>>>,
    circ_r: Arc<Vec<TmCell<f64>>>,
    min_angle: Arc<Vec<TmCell<f64>>>,
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
    fn test_circumcircle_right_triangle() {
        let (cx, cy, cr) = circumcircle(
            Point { x: 0.0, y: 0.0 },
            Point { x: 1.0, y: 0.0 },
            Point { x: 0.0, y: 1.0 },
        );
        assert!((cx - 0.5).abs() < 1e-10, "cx = {}", cx);
        assert!((cy - 0.5).abs() < 1e-10, "cy = {}", cy);
        let expected_r = (2.0f64).sqrt() / 2.0;
        assert!((cr - expected_r).abs() < 1e-10, "cr = {}, expected = {}", cr, expected_r);
    }

    #[test]
    fn test_tri_min_angle_equilateral() {
        let a = Point { x: 0.0, y: 0.0 };
        let b = Point { x: 1.0, y: 0.0 };
        let c = Point { x: 0.5, y: (3.0f64).sqrt() / 2.0 };
        let angle = tri_min_angle(a, b, c);
        assert!((angle - 60.0).abs() < 1e-10, "angle = {}, expected 60", angle);
    }

    #[test]
    fn test_tri_min_angle_right() {
        let a = Point { x: 0.0, y: 0.0 };
        let b = Point { x: 1.0, y: 0.0 };
        let c = Point { x: 0.0, y: 1.0 };
        let angle = tri_min_angle(a, b, c);
        assert!((angle - 45.0).abs() < 1e-10, "angle = {}, expected 45", angle);
    }

    #[test]
    fn test_is_encroached() {
        let edge_a = Point { x: 0.0, y: 0.0 };
        let edge_b = Point { x: 2.0, y: 0.0 };
        assert!(is_encroached(edge_a, edge_b, Point { x: 1.0, y: 0.5 }));
        assert!(!is_encroached(edge_a, edge_b, Point { x: 1.0, y: 1.5 }));
        assert!(is_encroached(edge_a, edge_b, Point { x: 1.0, y: 1.0 }));
    }
}

impl Clone for SharedData {
    fn clone(&self) -> Self {
        SharedData {
            encroached: self.encroached.clone(),
            is_garbage: self.is_garbage.clone(),
            is_referenced: self.is_referenced.clone(),
            nbr_cnt: self.nbr_cnt.clone(),
            nbr_data: self.nbr_data.clone(),
            work_heap: self.work_heap.clone(),
            work_heap_cnt: self.work_heap_cnt.clone(),
            elem_count: self.elem_count.clone(),
            pt_x: self.pt_x.clone(),
            pt_y: self.pt_y.clone(),
            circ_x: self.circ_x.clone(),
            circ_y: self.circ_y.clone(),
            circ_r: self.circ_r.clone(),
            min_angle: self.min_angle.clone(),
        }
    }
}
