use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::cell::RefCell;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== Labyrinth ===");
    println!("  Grid: {}x{}x{}", config.grid_x, config.grid_y, config.grid_z);
    let size = config.grid_x * config.grid_y * config.grid_z;
    let grid: Vec<TmCell<i32>> = (0..size).map(|_| TmCell::new(0)).collect();
    let grid = Arc::new(grid);
    let dur = std::time::Duration::from_millis(config.duration as u64);

    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let g = grid.clone();
            let sc = stop;
            let so = ops;
            s.spawn(move || {
                let rng = RefCell::new(Rng::new(tid as u64 * 12345 + 42));
                while !sc.load(Ordering::Relaxed) {
                    let sx = (rng.borrow_mut().next() % config.grid_x as u64) as usize;
                    let sy = (rng.borrow_mut().next() % config.grid_y as u64) as usize;
                    let sz = (rng.borrow_mut().next() % config.grid_z as u64) as usize;
                    let dx = (rng.borrow_mut().next() % config.grid_x as u64) as usize;
                    let dy = (rng.borrow_mut().next() % config.grid_y as u64) as usize;
                    let dz = (rng.borrow_mut().next() % config.grid_z as u64) as usize;

                    let _ = transaction(|tx| {
                        fn idx(xs: usize, ys: usize, _zs: usize, x: usize, y: usize, z: usize) -> usize {
                            z * xs * ys + y * xs + x
                        }
                        let grid: &[TmCell<i32>] = &*g;
                        if tx.read(&grid[idx(config.grid_x, config.grid_y, config.grid_z, sx, sy, sz)]) != 0 { return false; }
                        if tx.read(&grid[idx(config.grid_x, config.grid_y, config.grid_z, dx, dy, dz)]) != 0 { return false; }

                        let capacity = config.grid_x * config.grid_y * config.grid_z;
                        let mut q = Vec::with_capacity(capacity);
                        q.push((sx, sy, sz, 0i32));
                        tx.write(&grid[idx(config.grid_x, config.grid_y, config.grid_z, sx, sy, sz)], 1);
                        let mut found = false;
                        let mut front = 0;
                        while front < q.len() && !found {
                            let (cx, cy, cz, dist) = q[front];
                            front += 1;
                            if dist > 10 { break; }
                            for (ddx, ddy, ddz) in [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)] {
                                let nx = cx.wrapping_add(ddx as usize);
                                let ny = cy.wrapping_add(ddy as usize);
                                let nz = cz.wrapping_add(ddz as usize);
                                if nx >= config.grid_x || ny >= config.grid_y || nz >= config.grid_z { continue; }
                                if nx == dx && ny == dy && nz == dz { found = true; break; }
                                let idx = idx(config.grid_x, config.grid_y, config.grid_z, nx, ny, nz);
                                if tx.read(&grid[idx]) == 0 {
                                    tx.write(&grid[idx], 1);
                                    q.push((nx, ny, nz, dist + 1));
                                }
                            }
                        }
                        found
                    });
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    println!("  Labyrinth complete");
}
