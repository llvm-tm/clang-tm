use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use tm::{transaction, TmCell};
use crate::Rng;
use super::Config;

pub fn run(config: &Config, stop: &AtomicBool, ops: &AtomicU64) {
    println!("\n=== KMeans ===");
    println!("  Points: {}  Clusters: {}  Dims: {}",
             config.points, config.clusters, config.dims);
    let points: Vec<Vec<f64>> = (0..config.dims).map(|d|
        (0..config.points).map(|p| {
            let mut rng = Rng::new((d * config.points + p + 42) as u64);
            (rng.next() % 1000) as f64 / 100.0
        }).collect()
    ).collect();
    let clusters: Vec<Vec<f64>> = (0..config.dims).map(|d|
        (0..config.clusters).map(|c| {
            let mut rng = Rng::new((d * config.clusters + c + 12345) as u64);
            (rng.next() % 1000) as f64 / 100.0
        }).collect()
    ).collect();
    let assignment: Vec<TmCell<i32>> = (0..config.points).map(|_| TmCell::new(-1)).collect();
    let new_count: Vec<TmCell<i32>> = (0..config.clusters).map(|_| TmCell::new(0)).collect();
    let new_sum: Vec<Vec<TmCell<f64>>> = (0..config.dims)
        .map(|_| (0..config.clusters).map(|_| TmCell::new(0.0f64)).collect()).collect();

    let state = Arc::new((points, clusters, assignment, new_count, new_sum));
    let dur = std::time::Duration::from_millis(config.duration as u64);
    let chunk = config.points / config.threads;
    let g_iters = AtomicU64::new(0);

    let t0 = std::time::Instant::now();
    std::thread::scope(|s| {
        for tid in 0..config.threads {
            let sref = state.clone();
            let sc = stop;
            let so = ops;
            let gi = &g_iters;
            let start = tid * chunk;
            let end = if tid == config.threads - 1 { config.points } else { start + chunk };
            s.spawn(move || {
                while !sc.load(Ordering::Relaxed) {
                    // Assignment phase (non-TM)
                    for p in start..end {
                        let mut best_cluster = 0i32;
                        let mut best_dist = f64::MAX;
                        for c in 0..config.clusters {
                            let mut dist = 0.0;
                            for d in 0..config.dims {
                                let diff = sref.0[d][p] - sref.1[d][c];
                                dist += diff * diff;
                            }
                            if dist < best_dist { best_dist = dist; best_cluster = c as i32; }
                        }
                        unsafe { *sref.2[p].ptr() = best_cluster; }
                    }

                    // Reset accumulators (non-TM)
                    for c in 0..config.clusters {
                        unsafe { *sref.3[c].ptr() = 0; }
                        for d in 0..config.dims { unsafe { *sref.4[d][c].ptr() = 0.0; } }
                    }

                    // Accumulate (TM)
                    transaction(|tx| {
                        for p in start..end {
                            let cl = unsafe { *sref.2[p].ptr() };
                            tx.write(&sref.3[cl as usize], tx.read(&sref.3[cl as usize]) + 1);
                            for d in 0..config.dims {
                                tx.write(&sref.4[d][cl as usize],
                                         tx.read(&sref.4[d][cl as usize]) + sref.0[d][p]);
                            }
                        }
                    });

                    gi.fetch_add(1, Ordering::Relaxed);
                    so.fetch_add(1, Ordering::Relaxed);
                }
            });
        }
        std::thread::sleep(dur);
        stop.store(true, Ordering::Relaxed);
    });
    let elapsed = t0.elapsed().as_millis() as u64;
    let iters = g_iters.load(Ordering::Relaxed);
    println!("  Iterations: {}  Elapsed: {} ms", iters, elapsed);
}
