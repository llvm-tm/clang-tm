use std::cell::Cell;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Barrier};
use std::time::Instant;
use tm::*;

#[allow(dead_code)]
fn sqrt_s(x: f64) -> f64 {
    if x <= 0.0 { return 0.0; }
    let mut s = x;
    for _ in 0..25 { s = (s + x / s) * 0.5; }
    s
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_threads = 4;
    let mut nclusters = 16;
    let mut ndims = 2;
    let mut npoints = 2048;
    let mut threshold = 0.0001;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-p" => { i += 1; num_threads = args[i].parse().unwrap(); }
            "-k" => { i += 1; nclusters = args[i].parse().unwrap(); }
            "-d" => { i += 1; ndims = args[i].parse().unwrap(); }
            "-n" => { i += 1; npoints = args[i].parse().unwrap(); }
            "-t" => { i += 1; threshold = args[i].parse().unwrap(); }
            _ => {}
        }
        i += 1;
    }

    println!("Points:    {}", npoints);
    println!("Dims:      {}", ndims);
    println!("Clusters:  {}", nclusters);
    println!("Threshold: {}", threshold);

    tm_init();

    // Row-major storage: points[i * ndims + d]
    let mut rng = fastrand::Rng::with_seed(42);
    let mut points_raw = Vec::with_capacity(npoints * ndims);
    for p in 0..npoints {
        let cluster = p % nclusters;
        for _d in 0..ndims {
            let u: f64 = rng.u64(..) as f64 / u64::MAX as f64;
            points_raw.push((-10.0 + u * 20.0) + cluster as f64 * 5.0);
        }
    }

    let mut centroids_raw = Vec::with_capacity(nclusters * ndims);
    for _c in 0..nclusters {
        for _d in 0..ndims {
            let u: f64 = rng.u64(..) as f64 / u64::MAX as f64;
            centroids_raw.push(-10.0 + u * 20.0);
        }
    }

    let points: Vec<TmCell<f64>> = points_raw.into_iter().map(TmCell::new).collect();
    let centroids: Vec<TmCell<f64>> = centroids_raw.into_iter().map(TmCell::new).collect();
    let assignments: Vec<TmCell<i32>> = (0..npoints).map(|_| TmCell::new(-1)).collect();
    let new_centers_sum: Vec<TmCell<f64>> = (0..nclusters * ndims).map(|_| TmCell::new(0.0)).collect();
    let new_centers_count: Vec<TmCell<i32>> = (0..nclusters).map(|_| TmCell::new(0)).collect();

    let data = Arc::new((points, centroids, assignments, new_centers_sum, new_centers_count));
    let barrier = Arc::new(Barrier::new(num_threads));
    let converged = Arc::new(AtomicBool::new(false));

    let start = Instant::now();

    std::thread::scope(|s| {
        for tid in 0..num_threads {
            let data = data.clone();
            let barrier = barrier.clone();
            let converged = converged.clone();

            s.spawn(move || {
                tm_init_thread();

                let nd = ndims;
                let nc = nclusters;
                let np = npoints;

                let local_sum: Vec<Cell<f64>> = (0..nc * nd).map(|_| Cell::new(0.0)).collect();
                let local_count: Vec<Cell<i32>> = (0..nc).map(|_| Cell::new(0)).collect();

                let mut max_iters = 100;

                loop {
                    if max_iters <= 0 || converged.load(Ordering::Relaxed) {
                        break;
                    }
                    max_iters -= 1;

                    for c in &local_count { c.set(0); }
                    for s in &local_sum { s.set(0.0); }

                    let chunk = (np + num_threads - 1) / num_threads;
                    let start_idx = tid * chunk;
                    let end_idx = std::cmp::min(start_idx + chunk, np);

                    // Accumulate phase: one TX per point
                    if start_idx < end_idx {
                        for p in start_idx..end_idx {
                            transaction(|tx| {
                                let mut best = -1i32;
                                let mut best_dist = f64::MAX;
                                for c in 0..nc {
                                    let mut dist = 0.0;
                                    for d in 0..nd {
                                        let pd = tx.read(&data.0[p * nd + d]);
                                        let cd = tx.read(&data.1[c * nd + d]);
                                        let diff = pd - cd;
                                        dist += diff * diff;
                                    }
                                    if dist < best_dist { best_dist = dist; best = c as i32; }
                                }
                                tx.write(&data.2[p], best);

                                let bc = best as usize;
                                if best >= 0 && bc < nc {
                                    local_count[bc].set(local_count[bc].get() + 1);
                                    for d in 0..nd {
                                        local_sum[bc * nd + d].set(
                                            local_sum[bc * nd + d].get()
                                            + tx.read(&data.0[p * nd + d]));
                                    }
                                }
                            });
                        }
                    }

                    barrier.wait();

                    // All threads: write local to shared arrays (racy, like plugin)
                    unsafe {
                        for c in 0..nc {
                            *data.4[c].ptr() += local_count[c].get();
                            for d in 0..nd {
                                *data.3[c * nd + d].ptr() += local_sum[c * nd + d].get();
                            }
                        }
                    }

                    barrier.wait();

                    // Thread 0: centroid update + convergence check
                    if tid == 0 {
                        let mut delta = 0.0;
                        for c in 0..nc {
                            let cnt = unsafe { *data.4[c].ptr() };
                            if cnt > 0 {
                                for d in 0..nd {
                                    let idx = c * nd + d;
                                    let s = unsafe { *data.3[idx].ptr() };
                                    let new_val = s / cnt as f64;
                                    let old_val = unsafe { *data.1[idx].ptr() };
                                    let diff = old_val - new_val;
                                    delta += diff * diff;
                                    unsafe { *data.1[idx].ptr() = new_val };
                                    unsafe { *data.3[idx].ptr() = 0.0 };
                                }
                            }
                            unsafe { *data.4[c].ptr() = 0 };
                        }
                        let _ = delta;

                        // delta = sqrt_s(delta / (nc * nd) as f64);
                        // if delta < threshold {
                        //     converged.store(true, Ordering::Relaxed);
                        // }
                        // Disable convergence check — always run max_iters
                    }

                    barrier.wait();
                }

                tm_exit_thread();
            });
        }
    });

    let elapsed = start.elapsed();
    println!("    Elapsed = {} ms", elapsed.as_millis());

    tm_exit();
}
