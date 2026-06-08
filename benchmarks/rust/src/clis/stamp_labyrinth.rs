use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tm::*;
use benchmarks::Rng;

#[derive(Clone, Copy, PartialEq)]
struct Point3D {
    x: i32, y: i32, z: i32,
}

struct PathRequest {
    src: Point3D,
    dst: Point3D,
}

fn grid_idx(w: i32, _h: i32, x: i32, y: i32, z: i32) -> usize {
    (z * _h + y) as usize * w as usize + x as usize
}

#[allow(unused_variables)]
fn do_expansion(
    dist: &mut [i32],
    cell_states: &[i32],
    w: i32, h: i32, d: i32,
    src: Point3D, dst: Point3D,
    queue: &mut [usize],
) -> bool {
    dist.fill(-1);

    let mut qh = 0;
    let mut qt = 0;
    let idx = grid_idx(w, h, src.x, src.y, src.z);
    dist[idx] = 0;
    queue[qt] = idx;
    qt += 1;

    let dirs = [[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]];

    while qh < qt {
        let cur = queue[qh];
        qh += 1;
        let cx = (cur % w as usize) as i32;
        let cy = ((cur / w as usize) % h as usize) as i32;
        let cz = (cur / (w as usize * h as usize)) as i32;

        if cx == dst.x && cy == dst.y && cz == dst.z {
            return true;
        }

        for dir in &dirs {
            let nx = cx + dir[0];
            let ny = cy + dir[1];
            let nz = cz + dir[2];
            if nx < 0 || nx >= w || ny < 0 || ny >= h || nz < 0 || nz >= d { continue; }
            let nidx = grid_idx(w, h, nx, ny, nz);
            if cell_states[nidx] == -2 { continue; }
            if dist[nidx] == -1 {
                dist[nidx] = dist[cur] + 1;
                queue[qt] = nidx;
                qt += 1;
            }
        }
    }
    false
}

#[allow(unused_variables)]
fn do_traceback(
    path: &mut Vec<Point3D>,
    dist: &[i32],
    w: i32, h: i32, d: i32,
    src: Point3D, dst: Point3D,
) -> bool {
    path.clear();
    let mut cx = dst.x;
    let mut cy = dst.y;
    let mut cz = dst.z;
    let dirs = [[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]];
    let sidx = grid_idx(w, h, src.x, src.y, src.z);

    loop {
        path.push(Point3D { x: cx, y: cy, z: cz });
        let idx = grid_idx(w, h, cx, cy, cz);
        if idx == sidx { break; }

        let mut best_di = -1i32;
        let mut best_val = dist[idx];
        for (di, dir) in dirs.iter().enumerate() {
            let nx = cx + dir[0];
            let ny = cy + dir[1];
            let nz = cz + dir[2];
            if nx < 0 || nx >= w || ny < 0 || ny >= h || nz < 0 || nz >= d { continue; }
            let nv = dist[grid_idx(w, h, nx, ny, nz)];
            if nv >= 0 && nv < best_val { best_val = nv; best_di = di as i32; }
        }
        if best_di < 0 { path.clear(); return false; }
        let dir = dirs[best_di as usize];
        cx += dir[0]; cy += dir[1]; cz += dir[2];
    }
    path.reverse();
    !path.is_empty()
}

fn labyrinth_mark(
    grid: &[TmCell<i32>],
    w: i32, h: i32,
    path: &[Point3D],
) -> bool {
    transaction(|tx| {
        for i in 1..path.len().saturating_sub(1) {
            let idx = grid_idx(w, h, path[i].x, path[i].y, path[i].z);
            if tx.read(&grid[idx]) != -1 {
                return false;
            }
        }
        for i in 1..path.len().saturating_sub(1) {
            let idx = grid_idx(w, h, path[i].x, path[i].y, path[i].z);
            tx.write(&grid[idx], -2);
        }
        true
    })
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_threads = 4;
    let mut w = 32;
    let mut h = 32;
    let mut d = 3;
    let mut num_requests = 64;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-x" => { i += 1; w = args[i].parse().unwrap(); }
            "-y" => { i += 1; h = args[i].parse().unwrap(); }
            "-z" => { i += 1; d = args[i].parse().unwrap(); }
            "-n" => { i += 1; num_requests = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    println!("Maze size:    {}x{}x{}", w, h, d);
    println!("Paths to route: {}", num_requests);

    tm_init();

    let gridsize = (w * h * d) as usize;

    let grid: Vec<TmCell<i32>> = (0..gridsize).map(|_| TmCell::new(-1)).collect();

    let mut rng = Rng::new(42);
    let num_walls = gridsize / 8;

    // Place walls
    for _ in 0..num_walls {
        let idx = rng.next() as usize % gridsize;
        unsafe { *grid[idx].ptr() = -2; }
    }

    // Generate requests
    let mut requests: Vec<PathRequest> = Vec::with_capacity(num_requests);
    for _ in 0..num_requests {
        let (sx, sy, sz) = loop {
            let cx = (rng.next() as usize % w as usize) as i32;
            let cy = (rng.next() as usize % h as usize) as i32;
            let cz = (rng.next() as usize % d as usize) as i32;
            if unsafe { *grid[grid_idx(w, h, cx, cy, cz)].ptr() } == -1 { break (cx, cy, cz); }
        };
        let (dx, dy, dz) = loop {
            let cx = (rng.next() as usize % w as usize) as i32;
            let cy = (rng.next() as usize % h as usize) as i32;
            let cz = (rng.next() as usize % d as usize) as i32;
            let idx = grid_idx(w, h, cx, cy, cz);
            if unsafe { *grid[idx].ptr() } == -1 && !(cx == sx && cy == sy && cz == sz) { break (cx, cy, cz); }
        };
        requests.push(PathRequest { src: Point3D { x: sx, y: sy, z: sz }, dst: Point3D { x: dx, y: dy, z: dz } });
    }

    let requests = Arc::new(requests);
    let grid = Arc::new(grid);
    let total_ops = Arc::new(AtomicU64::new(0));

    let start = Instant::now();

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let grid = grid.clone();
            let requests = requests.clone();
            let total_ops = total_ops.clone();

            s.spawn(move || {
                tm_init_thread();

                let gridsize = (w * h * d) as usize;
                let mut local_grid = vec![0i32; gridsize];
                let mut dist = vec![0i32; gridsize];
                let mut queue = vec![0usize; gridsize];
                let mut path: Vec<Point3D> = Vec::new();

                let mut i = tid;
                while i < num_requests {
                    let req = &requests[i];

                    loop {
                        let lg_ptr = &mut local_grid[0] as *mut i32;
                        transaction(|tx| {
                            for g in 0..gridsize {
                                unsafe { *lg_ptr.add(g) = tx.read(&grid[g]); }
                            }
                        });

                        let ok = do_expansion(&mut dist, &local_grid, w, h, d, req.src, req.dst, &mut queue);
                        let traced = ok && do_traceback(&mut path, &dist, w, h, d, req.src, req.dst);
                        if !ok || !traced || path.is_empty() { break; }

                        let success = labyrinth_mark(&grid, w, h, &path);
                        if success { break; }
                    }

                    total_ops.fetch_add(1, Ordering::Relaxed);
                    i += num_threads;
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    let ops = total_ops.load(Ordering::Relaxed);

    println!("    Time = {} ms", elapsed.as_millis());
    println!("    Routed = {} / {}", ops, num_requests);

    tm_exit();
}
