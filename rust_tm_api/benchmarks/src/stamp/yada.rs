use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use tm::transaction;
use crate::Rng;
use super::Config;

// ── Geometry ──────────────────────────────────────────────
fn sq_dist(a: f64, b: f64, c: f64, d: f64) -> f64 {
    let dx = a - c;
    let dy = b - d;
    dx * dx + dy * dy
}

fn circumcircle(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> (f64, f64, f64) {
    let d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if d.abs() < 1e-15 { return (0.0, 0.0, 1e15); }
    let ccx = ((ax * ax + ay * ay) * (by - cy)
             + (bx * bx + by * by) * (cy - ay)
             + (cx * cx + cy * cy) * (ay - by)) / d;
    let ccy = ((ax * ax + ay * ay) * (cx - bx)
             + (bx * bx + by * by) * (ax - cx)
             + (cx * cx + cy * cy) * (bx - ax)) / d;
    let cr2 = sq_dist(ccx, ccy, ax, ay);
    (ccx, ccy, cr2.sqrt())
}

fn tri_min_angle(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> f64 {
    let abx = bx - ax; let aby = by - ay;
    let bcx = cx - bx; let bcy = cy - by;
    let cax = ax - cx; let cay = cy - ay;
    let la = (bcx * bcx + bcy * bcy).sqrt();
    let lb = (cax * cax + cay * cay).sqrt();
    let lc = (abx * abx + aby * aby).sqrt();
    let dot_a = (abx * cax + aby * cay) / (lc * lb);
    let dot_b = (bcx * abx + bcy * aby) / (la * lc);
    let dot_c = (cax * bcx + cay * bcy) / (lb * la);
    let aa = dot_a.clamp(-1.0, 1.0).acos() * 180.0 / std::f64::consts::PI;
    let ba = dot_b.clamp(-1.0, 1.0).acos() * 180.0 / std::f64::consts::PI;
    let ca = dot_c.clamp(-1.0, 1.0).acos() * 180.0 / std::f64::consts::PI;
    aa.min(ba).min(ca)
}

fn is_encroached(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> bool {
    let dx = bx - ax; let dy = by - ay;
    let cex2 = (ax + bx) / 2.0; let cy2 = (ay + by) / 2.0;
    let r2 = (dx * dx + dy * dy) / 4.0;
    let px = cx - cex2; let py = cy - cy2;
    px * px + py * py <= r2
}

fn point_in_circum_sq(cx: f64, cy: f64, px: f64, py: f64) -> f64 {
    let dx = px - cx; let dy = py - cy;
    dx * dx + dy * dy
}

fn make_edge(ax: f64, ay: f64, bx: f64, by: f64) -> (f64, f64, f64, f64) {
    let abit = ax.to_bits();
    let bbit = bx.to_bits();
    if abit < bbit || (abit == bbit && ay.to_bits() < by.to_bits()) {
        (ax, ay, bx, by)
    } else {
        (bx, by, ax, ay)
    }
}

fn edge_eq(a: &(f64, f64, f64, f64), b: &(f64, f64, f64, f64)) -> bool {
    a.0.to_bits() == b.0.to_bits() && a.1.to_bits() == b.1.to_bits() &&
    a.2.to_bits() == b.2.to_bits() && a.3.to_bits() == b.3.to_bits()
}

fn edges_share(ax: f64, ay: f64, bx: f64, by: f64, el: &Element) -> bool {
    for i in 0..3 {
        let (px, py) = (el.px[i], el.py[i]);
        let (qx, qy) = (el.px[(i + 1) % 3], el.py[(i + 1) % 3]);
        if (px.to_bits() == ax.to_bits() && py.to_bits() == ay.to_bits() && qx.to_bits() == bx.to_bits() && qy.to_bits() == by.to_bits()) ||
           (px.to_bits() == bx.to_bits() && py.to_bits() == by.to_bits() && qx.to_bits() == ax.to_bits() && qy.to_bits() == ay.to_bits()) {
            return true;
        }
    }
    false
}

// ── Element ───────────────────────────────────────────────
struct Element {
    px: [f64; 3], py: [f64; 3],
    circum_x: f64, circum_y: f64, circum_r: f64,
    min_angle: f64,
    encroached: u8, is_garbage: u8,
    neighbors: Vec<i32>,
}

fn new_element(ax: f64, ay: f64, bx: f64, by: f64, cx: f64, cy: f64) -> Element {
    let (ccx, ccy, cr) = circumcircle(ax, ay, bx, by, cx, cy);
    let ang = tri_min_angle(ax, ay, bx, by, cx, cy);
    let e = is_encroached(ax, ay, bx, by, cx, cy) ||
            is_encroached(bx, by, cx, cy, ax, ay) ||
            is_encroached(cx, cy, ax, ay, bx, by);
    Element {
        px: [ax, bx, cx], py: [ay, by, cy],
        circum_x: ccx, circum_y: ccy, circum_r: cr,
        min_angle: ang,
        encroached: if e { 1 } else { 0 }, is_garbage: 0,
        neighbors: Vec::new(),
    }
}

// ── Yada data ─────────────────────────────────────────────
struct YadaData {
    elements: Vec<Element>,
    max_elements: usize,
    work_heap_data: Vec<i32>,
    work_heap_count: i32,
}

// ── Grow cavity and retriangulate ─────────────────────────
fn grow_and_retriangulate(data: &mut YadaData, el_id: usize,
                          bef: &mut Vec<usize>,
                          border: &mut Vec<(f64, f64, f64, f64)>,
                          bad: &mut Vec<usize>) {
    let el = &data.elements[el_id];
    let (scx, scy, scr2) = (el.circum_x, el.circum_y, el.circum_r * el.circum_r);
    let (sp0x, sp0y, sp1x, sp1y, sp2x, sp2y) = (el.px[0], el.py[0], el.px[1], el.py[1], el.px[2], el.py[2]);

    bef.clear(); border.clear(); bad.clear();

    let mut queue = Vec::new();
    let mut visited = vec![false; data.elements.len()];
    bef.push(el_id); visited[el_id] = true; queue.push(el_id);

    while let Some(cur) = queue.pop() {
        for ni in 0..data.elements[cur].neighbors.len() {
            let nid = data.elements[cur].neighbors[ni] as usize;
            if nid >= data.elements.len() || visited[nid] { continue; }
            visited[nid] = true;
            if point_in_circum_sq(data.elements[nid].circum_x, data.elements[nid].circum_y, scx, scy) <= scr2 {
                bef.push(nid); queue.push(nid);
            } else {
                for j in 0..3 {
                    let (ax, ay) = (data.elements[nid].px[j], data.elements[nid].py[j]);
                    let (bx, by) = (data.elements[nid].px[(j+1)%3], data.elements[nid].py[(j+1)%3]);
                    let mut is_border = true;
                    for &bid in bef.iter() {
                        if edges_share(ax, ay, bx, by, &data.elements[bid]) { is_border = false; break; }
                    }
                    if is_border { border.push(make_edge(ax, ay, bx, by)); }
                }
            }
        }
    }

    for &bid in bef.iter() { data.elements[bid].is_garbage = 1; }

    let cex = sp0x / 3.0 + sp1x / 3.0 + sp2x / 3.0;
    let cey = sp0y / 3.0 + sp1y / 3.0 + sp2y / 3.0;

    let mut unique: Vec<(f64,f64,f64,f64)> = Vec::new();
    for e in border.iter() { if !unique.iter().any(|u| edge_eq(u, e)) { unique.push(*e); } }

    for &(x1, y1, x2, y2) in unique.iter().take(3) {
        let mut tri = new_element(cex, cey, x1, y1, x2, y2);
        tri.encroached = 0;
        data.elements.push(tri);
        let new_id = data.elements.len() - 1;
        if data.elements[new_id].min_angle < data.elements[el_id].min_angle || data.elements[new_id].encroached != 0 {
            bad.push(new_id);
        }
    }
}

// ── Run ───────────────────────────────────────────────────
pub fn run(config: &Config, stop: &AtomicBool, _ops: &AtomicU64) {
    println!("\n=== Yada ===");
    let grid_size: usize = config.points.max(10).min(30);
    let angle_constraint = config.dims as f64;
    let jitter = 0.5;
    println!("  Grid: {}×{}  Angle: {}°  Jitter: {:.2}", grid_size, grid_size, angle_constraint as i32, jitter);

    let spacing = 4.0;
    let mut rng = Rng::new(42);
    let mut pxs = Vec::new();
    let mut pys = Vec::new();
    for i in 0..grid_size {
        for j in 0..grid_size {
            let x = i as f64 * spacing + (rng.next() as f64 / u64::MAX as f64 - 0.5) * jitter * 2.0;
            let y = j as f64 * spacing + (rng.next() as f64 / u64::MAX as f64 - 0.5) * jitter * 2.0;
            pxs.push(x); pys.push(y);
        }
    }

    let max_elements = 200000;
    let mut data = Box::new(YadaData {
        elements: Vec::with_capacity(max_elements),
        max_elements,
        work_heap_data: vec![0; max_elements],
        work_heap_count: 0,
    });

    let stride = grid_size;
    for i in 0..grid_size - 1 {
        for j in 0..grid_size - 1 {
            let i0 = i * stride + j; let i1 = (i+1) * stride + j;
            let i2 = i * stride + (j+1); let i3 = (i+1) * stride + (j+1);
            data.elements.push(new_element(pxs[i0], pys[i0], pxs[i1], pys[i1], pxs[i3], pys[i3]));
            data.elements.push(new_element(pxs[i0], pys[i0], pxs[i3], pys[i3], pxs[i2], pys[i2]));
        }
    }
    println!("  Initial elements: {}", data.elements.len());

    let n_el = data.elements.len();
    for i in 0..n_el {
        for j in (i+1)..n_el {
            let mut shared = 0usize;
            for pi in 0..3 { for pj in 0..3 {
                if data.elements[i].px[pi].to_bits() == data.elements[j].px[pj].to_bits() &&
                   data.elements[i].py[pi].to_bits() == data.elements[j].py[pj].to_bits() { shared += 1; }
            }}
            if shared >= 2 {
                data.elements[i].neighbors.push(j as i32);
                data.elements[j].neighbors.push(i as i32);
            }
        }
    }

    let mut bad_count = 0;
    for i in 0..n_el {
        if data.elements[i].min_angle < angle_constraint || data.elements[i].encroached != 0 {
            let n = data.work_heap_count as usize;
            if n < max_elements { data.work_heap_data[n] = i as i32; data.work_heap_count = (n+1) as i32; }
            bad_count += 1;
        }
    }
    println!("  Initial bad: {}", bad_count);

    let dur = std::time::Duration::from_millis(config.duration as u64);
    let g_ops = AtomicU64::new(0);
    let t0 = std::time::Instant::now();

    // Store data in a Box, then use raw pointer across threads (only thread 0 touches it)
    let data_ptr = Box::into_raw(data);
    let ptr_atomic = Arc::new(AtomicUsize::new(data_ptr as usize));

    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let sc = stop;
            let go = &g_ops;
            let pa = Arc::clone(&ptr_atomic);
            s.spawn(move || {
                if tid != 0 {
                    while !sc.load(Ordering::Relaxed) { std::thread::yield_now(); }
                    return;
                }
                let ptr = pa.load(Ordering::Relaxed) as *mut YadaData;
                let mut bef: Vec<usize> = Vec::new();
                let mut border: Vec<(f64,f64,f64,f64)> = Vec::new();
                let mut bad: Vec<usize> = Vec::new();

                loop {
                    let el_id = transaction(|_tx| unsafe {
                        let dd = &mut *ptr;
                        let n = dd.work_heap_count as usize;
                        if n == 0 { return -1i32; }
                        let mut best = 0usize;
                        let mut best_enc = dd.elements[dd.work_heap_data[0] as usize].encroached;
                        for i in 1..n {
                            let eid = dd.work_heap_data[i] as usize;
                            let enc = dd.elements[eid].encroached;
                            if enc > best_enc { best_enc = enc; best = i; }
                        }
                        let last = n - 1;
                        let el = dd.work_heap_data[best];
                        if best != last { dd.work_heap_data[best] = dd.work_heap_data[last]; }
                        dd.work_heap_count = last as i32;
                        el
                    });
                    if el_id < 0 || sc.load(Ordering::Relaxed) { break; }
                    if el_id >= 0 {
                        let dd = unsafe { &*ptr };
                        if dd.elements[el_id as usize].is_garbage != 0 { continue; }
                    }
                    unsafe { grow_and_retriangulate(&mut *ptr, el_id as usize, &mut bef, &mut border, &mut bad); }
                    if !bad.is_empty() {
                        unsafe {
                            let dd = &mut *ptr;
                            for &bid in bad.iter() {
                                let n = dd.work_heap_count as usize;
                                if n < dd.max_elements {
                                    dd.work_heap_data[n] = bid as i32;
                                    dd.work_heap_count = (n+1) as i32;
                                }
                            }
                        }
                    }
                    go.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });

    let elapsed = t0.elapsed().as_millis() as u64;
    let ops_count = g_ops.load(Ordering::Relaxed);
    let total_el = unsafe { (*data_ptr).elements.len() };
    println!("  Operations: {}  Total elements: {}  Elapsed: {} ms", ops_count, total_el, elapsed);
    drop(unsafe { Box::from_raw(data_ptr) });
}
