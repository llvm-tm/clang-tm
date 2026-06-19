use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn test() -> i32 {
    let mut fails = 0;
    // BFS always finds path in empty grid
    let w = 3; let h = 3; let d = 1;
    let size = w * h * d;
    let g: Vec<TmCell<i32>> = (0..size).map(|_| TmCell::new(0)).collect();
    let src = 0usize; let dst = size - 1;
    let path = {
        let local: Vec<i32> = (0..size).map(|i| unsafe { *g[i].ptr() }).collect();
        let mut visited = vec![false; size];
        let mut queue = std::collections::VecDeque::new();
        let mut parent = vec![usize::MAX; size];
        visited[src] = true; queue.push_back(src);
        while let Some(cur) = queue.pop_front() {
            if cur == dst { break; }
            let (cx, cy, cz) = (cur % w, (cur / w) % h, cur / (w * h));
            for (dx, dy, dz) in [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)] {
                let nx = cx.wrapping_add_signed(dx);
                let ny = cy.wrapping_add_signed(dy);
                let nz = cz.wrapping_add_signed(dz);
                if nx < w && ny < h && nz < d {
                    let ni = nx + ny * w + nz * w * h;
                    if !visited[ni] && local[ni] == 0 {
                        visited[ni] = true; parent[ni] = cur; queue.push_back(ni);
                    }
                }
            }
        }
        if !visited[dst] { Vec::new() } else {
            let mut p = Vec::new(); let mut cur = dst;
            while cur != usize::MAX { p.push(cur as i32); cur = parent[cur]; }
            p.reverse(); p
        }
    };
    if path.is_empty() { eprintln!("FAIL: empty grid BFS found no path"); fails += 1; }
    if path.len() < 5 { eprintln!("FAIL: path too short: {}", path.len()); fails += 1; }

    // Wall blocking middle row
    for col in 0..w { unsafe { *g[1 * w + col].ptr() = 1; } }
    let blocked = {
        let local: Vec<i32> = (0..size).map(|i| unsafe { *g[i].ptr() }).collect();
        let mut visited = vec![false; size];
        let mut queue = std::collections::VecDeque::new();
        visited[src] = true; queue.push_back(src);
        while let Some(cur) = queue.pop_front() {
            if cur == dst { break; }
            let (cx, cy, cz) = (cur % w, (cur / w) % h, cur / (w * h));
            for (dx, dy, dz) in [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)] {
                let nx = cx.wrapping_add_signed(dx);
                let ny = cy.wrapping_add_signed(dy);
                let nz = cz.wrapping_add_signed(dz);
                if nx < w && ny < h && nz < d {
                    let ni = nx + ny * w + nz * w * h;
                    if !visited[ni] && local[ni] == 0 {
                        visited[ni] = true; queue.push_back(ni);
                    }
                }
            }
        }
        !visited[dst]
    };
    if !blocked { eprintln!("FAIL: obstacle should block path"); fails += 1; }

    if fails > 0 { eprintln!("labyrinth: {} test(s) failed", fails); }
    fails
}

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Labyrinth ===");
    println!("  Grid: {}x{}x{}", config.grid_x, config.grid_y, config.grid_z);
    let size = config.grid_x * config.grid_y * config.grid_z;
    let grid: Vec<TmCell<i32>> = (0..size).map(|_| TmCell::new(0)).collect();
    let grid = Arc::new(grid);
    let dur = std::time::Duration::from_millis(config.duration as u64);

    let g_done = AtomicU64::new(0);

    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let g = grid.clone();
            let sc = stop;
            let so = ops;
            let gd = &g_done;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                while !sc.load(Ordering::Relaxed) {
                    let sx = (rng.borrow_mut().next() % config.grid_x as u64) as usize;
                    let sy = (rng.borrow_mut().next() % config.grid_y as u64) as usize;
                    let sz = (rng.borrow_mut().next() % config.grid_z as u64) as usize;
                    let dx = (rng.borrow_mut().next() % config.grid_x as u64) as usize;
                    let dy = (rng.borrow_mut().next() % config.grid_y as u64) as usize;
                    let dz = (rng.borrow_mut().next() % config.grid_z as u64) as usize;

                    fn idx(xs: usize, ys: usize, _zs: usize, x: usize, y: usize, z: usize) -> usize {
                        z * xs * ys + y * xs + x
                    }

                    // Read grid snapshot (non-TM may be stale)
                    let local: Vec<i32> = (0..size).map(|i| unsafe { *g[i].ptr() }).collect();
                    let w = config.grid_x;
                    let h = config.grid_y;
                    let d = config.grid_z;

                    // BFS on local copy
                    let mut q: Vec<(usize, usize, usize)> = Vec::with_capacity(size);
                    let mut dist: Vec<i32> = vec![-1i32; size];
                    let start_i = idx(w, h, d, sx, sy, sz);
                    let dst_i = idx(w, h, d, dx, dy, dz);
                    if local[start_i] != 0 || local[dst_i] != 0 { continue; }

                    dist[start_i] = 0;
                    q.push((sx, sy, sz));
                    let mut front = 0;
                    while front < q.len() {
                        let (cx, cy, cz) = q[front];
                        front += 1;
                        if cx == dx && cy == dy && cz == dz { break; }
                        for &(ddx, ddy, ddz) in &[(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)] {
                            let nx = cx.wrapping_add(ddx as usize);
                            let ny = cy.wrapping_add(ddy as usize);
                            let nz = cz.wrapping_add(ddz as usize);
                            if nx >= w || ny >= h || nz >= d { continue; }
                            let ni = idx(w, h, d, nx, ny, nz);
                            if local[ni] == 0 && dist[ni] < 0 {
                                dist[ni] = dist[idx(w, h, d, cx, cy, cz)] + 1;
                                q.push((nx, ny, nz));
                            }
                        }
                    }
                    if dist[dst_i] < 0 { continue; }

                    // Traceback (non-TM)
                    let mut path: Vec<(usize, usize, usize)> = Vec::new();
                    let (mut cx, mut cy, mut cz) = (dx, dy, dz);
                    loop {
                        path.push((cx, cy, cz));
                        if cx == sx && cy == sy && cz == sz { break; }
                        let mut best = (0i32, 0usize, 0usize, 0usize);
                        for &(ddx, ddy, ddz) in &[(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)] {
                            let nx = cx.wrapping_add(ddx as usize);
                            let ny = cy.wrapping_add(ddy as usize);
                            let nz = cz.wrapping_add(ddz as usize);
                            if nx >= w || ny >= h || nz >= d { continue; }
                            let nd = dist[idx(w, h, d, nx, ny, nz)];
                            if nd >= 0 && (best.0 < 0 || nd < best.0) {
                                best = (nd, nx, ny, nz);
                            }
                        }
                        if best.0 < 0 { path.clear(); break; }
                        cx = best.1; cy = best.2; cz = best.3;
                    }
                    if path.is_empty() { continue; }
                    path.reverse();

                    // TX-mark: atomically verify and mark path cells
                    let ok = transaction(|tx| {
                        for i in 1..path.len().saturating_sub(1) {
                            let pi = idx(w, h, d, path[i].0, path[i].1, path[i].2);
                            if tx.read(&g[pi]) != 0 { return false; }
                        }
                        for i in 1..path.len().saturating_sub(1) {
                            let pi = idx(w, h, d, path[i].0, path[i].1, path[i].2);
                            tx.write(&g[pi], 1);
                        }
                        true
                    });
                    if ok { gd.fetch_add(1, Ordering::Relaxed); }
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = g_done.load(Ordering::Relaxed);
    println!("  Paths routed: {}  Operations: {}", elapsed, ops.load(Ordering::Relaxed));
}
